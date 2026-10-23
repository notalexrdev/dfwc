/* See LICENSE file for copyright and license details.
 *
 * init.lua is loaded at startup and again whenever a .lua file next to it is
 * saved. Each load gets a fresh Lua state; a config that fails to load never
 * replaces a working one.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <lauxlib.h>
#include <lualib.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>

#include "dfwc.h"

#define LENGTH(X) (sizeof(X) / sizeof((X)[0]))

typedef struct {
	uint32_t mods;
	xkb_keysym_t sym;
	int fn; /* registry ref */
} Bind;

typedef struct {
	lua_State *L;
	Bind *binds;
	size_t nbinds;
} Config;

static const char defconfig[] = {
#embed "init.lua"
};

static Config cur, load; /* running config, config being loaded */
static char dir[PATH_MAX], path[PATH_MAX + sizeof("/init.lua")];
static struct wl_event_loop *evloop;
static struct wl_event_source *pending; /* scheduled reload */
static struct wl_event_source *watcher;
static int watchfd = -1;

static int
parsekeys(const char *keys, uint32_t *mods, xkb_keysym_t *sym)
{
	static const struct { const char *name; uint32_t mask; } modnames[] = {
		{ "Mod", MODKEY_BIT }, { "Super", WLR_MODIFIER_LOGO }, { "Shift", WLR_MODIFIER_SHIFT },
		{ "Control", WLR_MODIFIER_CTRL }, { "Ctrl", WLR_MODIFIER_CTRL }, { "Alt", WLR_MODIFIER_ALT },
	};
	char buf[256], *tok, *next, *save;
	size_t i;

	if (snprintf(buf, sizeof(buf), "%s", keys) >= (int)sizeof(buf))
		return 0;
	*mods = 0;
	/* every token is a modifier except the last, which is the key */
	for (tok = strtok_r(buf, "+", &save); tok; tok = next) {
		if (!(next = strtok_r(NULL, "+", &save))) {
			*sym = xkb_keysym_to_lower(xkb_keysym_from_name(tok, XKB_KEYSYM_CASE_INSENSITIVE));
			return *sym != XKB_KEY_NoSymbol;
		}
		for (i = 0; i < LENGTH(modnames) && strcasecmp(tok, modnames[i].name); i++);
		if (i == LENGTH(modnames))
			return 0;
		*mods |= modnames[i].mask;
	}
	return 0;
}

static int
l_bind(lua_State *L)
{
	/* binds made by a running callback go into the running config */
	Config *c = L == load.L ? &load : &cur;
	const char *keys = luaL_checkstring(L, 1);
	Bind b, *binds;

	luaL_checktype(L, 2, LUA_TFUNCTION);
	if (!parsekeys(keys, &b.mods, &b.sym))
		return luaL_error(L, "bad key \"%s\"", keys);
	if (!(binds = realloc(c->binds, (c->nbinds + 1) * sizeof(*binds))))
		return luaL_error(L, "out of memory");
	lua_pushvalue(L, 2);
	b.fn = luaL_ref(L, LUA_REGISTRYINDEX);
	c->binds = binds;
	c->binds[c->nbinds++] = b;
	return 0;
}

static int
l_spawn(lua_State *L)
{
	spawn(luaL_checkstring(L, 1));
	return 0;
}

static int
l_close(lua_State *L)
{
	killclient();
	return 0;
}

static int
l_quit(lua_State *L)
{
	quit();
	return 0;
}

static int
l_reload(lua_State *L)
{
	config_reload();
	return 0;
}

static const luaL_Reg api[] = {
	{ "bind", l_bind },
	{ "spawn", l_spawn },
	{ "close", l_close },
	{ "quit", l_quit },
	{ "reload", l_reload },
	{ NULL, NULL },
};

static void
freeconfig(Config *c)
{
	if (c->L)
		lua_close(c->L);
	free(c->binds);
	*c = (Config){ 0 };
}

static int
loadconfig(const char *file)
{
	/* file NULL loads the built-in default */
	lua_State *L = luaL_newstate();

	if (!L) {
		wlr_log(WLR_ERROR, "config: out of memory");
		return 0;
	}
	load = (Config){ .L = L };
	luaL_openlibs(L);
	luaL_newlib(L, api);
	lua_setglobal(L, "dfwc");
	/* let init.lua require() files next to it */
	lua_getglobal(L, "package");
	lua_pushfstring(L, "%s/?.lua;%s/?/init.lua;", dir, dir);
	lua_getfield(L, -2, "path");
	lua_concat(L, 2);
	lua_setfield(L, -2, "path");
	lua_pop(L, 1);

	if ((file ? luaL_loadfile(L, file)
	          : luaL_loadbuffer(L, defconfig, sizeof(defconfig), "=default init.lua")) != LUA_OK
	|| lua_pcall(L, 0, 0, 0) != LUA_OK) {
		wlr_log(WLR_ERROR, "config: %s", lua_tostring(L, -1));
		freeconfig(&load);
		return 0;
	}
	freeconfig(&cur);
	cur = load;
	load = (Config){ 0 };
	return 1;
}

bool
config_key(uint32_t mods, xkb_keysym_t sym)
{
	/* newest bind wins, so later binds override earlier ones */
	for (size_t i = cur.nbinds; i-- > 0;) {
		Bind *b = &cur.binds[i];
		if (b->sym != sym || ((b->mods & ~MODKEY_BIT) | (b->mods & MODKEY_BIT ? modkey : 0)) != mods)
			continue;
		lua_rawgeti(cur.L, LUA_REGISTRYINDEX, b->fn);
		if (lua_pcall(cur.L, 0, 0, 0) != LUA_OK) {
			wlr_log(WLR_ERROR, "config: %s", lua_tostring(cur.L, -1));
			lua_pop(cur.L, 1);
		}
		return true;
	}
	return false;
}

static void
doreload(void *data)
{
	pending = NULL;
	if (loadconfig(path))
		wlr_log(WLR_INFO, "config: reloaded %s", path);
}

void
config_reload(void)
{
	/* deferred: a bind calling dfwc.reload must not free the state it runs in */
	if (!pending)
		pending = wl_event_loop_add_idle(evloop, doreload, NULL);
}

static int
watch(int fd, uint32_t mask, void *data)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	struct inotify_event *ev;
	ssize_t len;
	size_t n;

	while ((len = read(fd, buf, sizeof(buf))) > 0)
		for (char *p = buf; p < buf + len; p += sizeof(*ev) + ev->len) {
			ev = (struct inotify_event *)p;
			n = ev->len ? strlen(ev->name) : 0;
			if (n > 4 && !strcmp(ev->name + n - 4, ".lua"))
				config_reload();
		}
	return 0;
}

void
config_init(struct wl_event_loop *loop)
{
	const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	int fd;

	if (xdg && *xdg)
		snprintf(dir, sizeof(dir), "%s/dfwc", xdg);
	else
		snprintf(dir, sizeof(dir), "%s/.config/dfwc", home ? home : "");
	snprintf(path, sizeof(path), "%s/init.lua", dir);
	evloop = loop;
	if (access(path, F_OK) || !loadconfig(path))
		loadconfig(NULL);

	/* shortcut: the directory must exist at startup to be watched; watch its
	 * parent too if creating it while dfwc runs should start live reloads */
	if ((fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) < 0
	|| inotify_add_watch(fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
		wlr_log(WLR_INFO, "config: not watching %s for changes", dir);
		if (fd >= 0)
			close(fd);
		return;
	}
	watchfd = fd;
	watcher = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, watch, NULL);
}

void
config_finish(void)
{
	if (pending)
		wl_event_source_remove(pending);
	if (watcher)
		wl_event_source_remove(watcher);
	if (watchfd >= 0)
		close(watchfd);
	freeconfig(&cur);
}

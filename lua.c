/* See LICENSE file for copyright and license details.
 *
 * init.lua is loaded at startup and again whenever a .lua file next to it is
 * saved. Each load gets a fresh Lua state; a config that fails to load never
 * replaces a working one.
 */
#include <limits.h>
#include <stddef.h>
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
	bool repeat;
} Bind;

typedef struct {
	lua_State *L;
	Bind *binds;
	size_t nbinds;
	char **autostart;
	size_t nautostart;
	int style;   /* registry ref of the dfwc.style() function, or LUA_NOREF */
	int layouts; /* registry ref of the table of dfwc.addlayout() functions */
	Settings settings;
} Config;

enum { Str, Int, Num, Bool, Color, Mod, Sides };

#define FIELD(N, T, MIN, MAX) { #N, T, offsetof(Settings, N), sizeof(((Settings *)0)->N), MIN, MAX, NULL }
#define CHOICE(N, ...) { #N, Str, offsetof(Settings, N), sizeof(((Settings *)0)->N), 0, 0, \
	(const char *const[]){ __VA_ARGS__, NULL } }
static const struct {
	const char *name;
	int type;
	size_t off, size;
	double min, max;
	const char *const *choices; /* for strings with a fixed set of values */
} fields[] = {
	FIELD(xkb_layout, Str, 0, 0),
	FIELD(xkb_variant, Str, 0, 0),
	FIELD(xkb_options, Str, 0, 0),
	FIELD(xkb_model, Str, 0, 0),
	FIELD(repeat_rate, Int, 0, 1000),
	FIELD(repeat_delay, Int, 0, 10000),
	FIELD(touchpad_tap, Bool, 0, 0),
	FIELD(touchpad_natural_scroll, Bool, 0, 0),
	FIELD(touchpad_dwt, Bool, 0, 0),
	FIELD(touchpad_speed, Num, -1, 1),
	FIELD(cursor_theme, Str, 0, 0),
	FIELD(cursor_size, Int, 8, 256),
	FIELD(background, Color, 0, 0),
	FIELD(scale, Num, 0.25, 8),
	FIELD(modkey, Mod, 0, 0),
	FIELD(border_width, Sides, 0, 100),
	FIELD(border_focused, Color, 0, 0),
	FIELD(border_unfocused, Color, 0, 0),
	FIELD(inner_border_width, Int, 0, 100),
	FIELD(inner_border_focused, Color, 0, 0),
	FIELD(inner_border_unfocused, Color, 0, 0),
	FIELD(border_gap, Int, 0, 100),
	FIELD(border_corners, Int, 0, 1000),
	FIELD(corner_radius, Int, 0, 1000),
	FIELD(opacity, Num, 0.05, 1),
	FIELD(error_alarm_fullscreen, Bool, 0, 0),
	FIELD(launcher, Str, 0, 0),
	FIELD(restore_menu, Str, 0, 0),
	FIELD(box_timeout, Int, 0, 60000),
	FIELD(snap, Int, 0, 200),
	FIELD(tiling, Bool, 0, 0),
	FIELD(layout, Str, 0, 0),
	FIELD(mfact, Num, 0.05, 0.95),
	FIELD(nmaster, Int, 0, 20),
	FIELD(gaps_inner, Int, 0, 200),
	FIELD(gaps_outer, Int, 0, 200),
	FIELD(smart_gaps, Bool, 0, 0),
	CHOICE(tiled_drag, "swap", "snap", "float"),
	FIELD(wallpaper, Bool, 0, 0),
};

/* runtime changes to these survive a save, unless the config's value for them changed */
#define KEEP(N) { offsetof(Settings, N), sizeof(((Settings *)0)->N) }
static const struct { size_t off, size; } keep[] = {
	KEEP(tiling), KEEP(layout), KEEP(mfact), KEEP(nmaster),
};

static const char *const layouts[] = {
	"tile", "bstack", "spiral", "dwindle", "centeredmaster", "grid", "monocle", NULL,
};

#define HEX(C) { ((C) >> 16 & 0xff) / 255.0f, ((C) >> 8 & 0xff) / 255.0f, ((C) & 0xff) / 255.0f, 1 }

static const Settings defaults = {
	.repeat_rate = 25, .repeat_delay = 600,
	.touchpad_tap = true, .touchpad_dwt = true,
	.cursor_size = 24,
	/* dfwm's tomorrow-night colors */
	.background = HEX(0x1d1f21),
	.scale = 1,
	.border_width = { 2, 2, 2, 2 },
	.border_focused = HEX(0x9a9ea3),
	.border_unfocused = HEX(0x3c3f43),
	.inner_border_focused = HEX(0x1d1f21),
	.inner_border_unfocused = HEX(0x1d1f21),
	.opacity = 1,
	.launcher = "wmenu-run",
	.restore_menu = "wmenu -p restore",
	.box_timeout = 5000,
	.snap = 10,
	.layout = "tile",
	.mfact = 0.55,
	.nmaster = 1,
	.tiled_drag = "swap",
	.wallpaper = true,
};

static const struct { const char *name; uint32_t mask; } modnames[] = {
	{ "Mod", MODKEY_BIT }, { "Super", WLR_MODIFIER_LOGO }, { "Shift", WLR_MODIFIER_SHIFT },
	{ "Control", WLR_MODIFIER_CTRL }, { "Ctrl", WLR_MODIFIER_CTRL }, { "Alt", WLR_MODIFIER_ALT },
};

Settings settings;

static const char defconfig[] = {
#embed "init.lua"
};

static Config cur = { .style = LUA_NOREF, .layouts = LUA_NOREF },
	load = { .style = LUA_NOREF, .layouts = LUA_NOREF }; /* running, loading */
static char dir[PATH_MAX], path[PATH_MAX + sizeof("/init.lua")];
static struct wl_event_loop *evloop;
static struct wl_event_source *pending; /* scheduled reload */
static struct wl_event_source *watcher;
static int watchfd = -1;
static char **started; /* autostart commands already run this session */
static size_t nstarted;

static int
parsekeys(const char *keys, uint32_t *mods, xkb_keysym_t *sym)
{
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
	b.repeat = false;
	if (!lua_isnoneornil(L, 3)) { /* options: { repeats = true } ("repeat" is a Lua keyword) */
		luaL_checktype(L, 3, LUA_TTABLE);
		b.repeat = lua_getfield(L, 3, "repeats") != LUA_TNIL && lua_toboolean(L, -1);
		lua_pop(L, 1);
	}
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
parsecolor(const char *s, float c[4])
{
	size_t n = strlen(s);
	unsigned long v;

	if (s[0] != '#' || (n != 7 && n != 9) || strspn(s + 1, "0123456789abcdefABCDEF") != n - 1)
		return 0;
	v = strtoul(s + 1, NULL, 16);
	if (n == 7)
		v = v << 8 | 0xff;
	for (int i = 0; i < 4; i++)
		c[i] = (v >> (24 - 8 * i) & 0xff) / 255.0f;
	return 1;
}

static int
l_set(lua_State *L)
{
	/* settings made by a running callback apply right away */
	Settings *s = L == load.L ? &load.settings : &settings, tmp = *s;
	const char *key, *str;
	size_t i, len;
	lua_Integer n;
	double d;
	int ok;
	void *p;

	luaL_checktype(L, 1, LUA_TTABLE);
	lua_pushnil(L);
	while (lua_next(L, 1)) {
		key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "";
		for (i = 0; i < LENGTH(fields) && strcmp(key, fields[i].name); i++);
		if (i == LENGTH(fields))
			return luaL_error(L, "unknown setting \"%s\"", key);
		p = (char *)&tmp + fields[i].off;
		ok = 1;
		switch (fields[i].type) {
		case Str:
			if ((ok = lua_type(L, -1) == LUA_TSTRING)) {
				str = lua_tolstring(L, -1, &len);
				if (len >= fields[i].size)
					return luaL_error(L, "%s: too long", key);
				for (const char *const *c = fields[i].choices; c && *c; c++)
					if (!strcmp(*c, str))
						goto chosen;
				if (fields[i].choices) { /* "tiled_drag: use swap, snap or float" */
					luaL_Buffer b;
					luaL_buffinit(L, &b);
					luaL_where(L, 1);
					luaL_addvalue(&b);
					luaL_addstring(&b, key);
					luaL_addstring(&b, ": use");
					for (const char *const *c = fields[i].choices; *c; c++) {
						luaL_addstring(&b, c == fields[i].choices ? " " : c[1] ? ", " : " or ");
						luaL_addstring(&b, *c);
					}
					luaL_pushresult(&b);
					return lua_error(L);
				}
			chosen:
				memcpy(p, str, len + 1);
			}
			break;
		case Int:
			n = lua_tointegerx(L, -1, &ok);
			ok = ok && n >= fields[i].min && n <= fields[i].max;
			if (ok)
				*(int *)p = n;
			break;
		case Num:
			d = lua_tonumberx(L, -1, &ok);
			ok = ok && d >= fields[i].min && d <= fields[i].max;
			if (ok)
				*(double *)p = d;
			break;
		case Bool:
			if ((ok = lua_isboolean(L, -1)))
				*(bool *)p = lua_toboolean(L, -1);
			break;
		case Color:
			ok = lua_type(L, -1) == LUA_TSTRING && parsecolor(lua_tostring(L, -1), p);
			break;
		case Sides: /* one width, or { top, right, bottom, left } */
			if (lua_istable(L, -1)) {
				for (int j = 0; ok && j < 4; j++) {
					lua_rawgeti(L, -1, j + 1);
					n = lua_tointegerx(L, -1, &ok);
					ok = ok && n >= fields[i].min && n <= fields[i].max;
					((int *)p)[j] = n;
					lua_pop(L, 1);
				}
			} else {
				n = lua_tointegerx(L, -1, &ok);
				ok = ok && n >= fields[i].min && n <= fields[i].max;
				for (int j = 0; ok && j < 4; j++)
					((int *)p)[j] = n;
			}
			break;
		case Mod:
			str = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
			*(uint32_t *)p = 0;
			if (strcmp(str, "auto")) {
				for (len = 1; len < LENGTH(modnames) && strcasecmp(str, modnames[len].name); len++);
				if ((ok = len < LENGTH(modnames)))
					*(uint32_t *)p = modnames[len].mask;
			}
			break;
		}
		if (!ok)
			return luaL_error(L, "%s: bad value", key);
		lua_pop(L, 1);
	}
	*s = tmp;
	if (s == &settings)
		applysettings();
	return 0;
}

static void
autostart(const char *cmd)
{
	/* each command runs once per session, so saving init.lua never restarts
	 * an app but a newly added autostart line starts right away */
	char **ran;
	size_t i;

	for (i = 0; i < nstarted && strcmp(started[i], cmd); i++);
	if (i < nstarted || !(ran = realloc(started, (nstarted + 1) * sizeof(*ran))))
		return;
	started = ran;
	if ((started[nstarted] = strdup(cmd))) {
		nstarted++;
		spawn(cmd);
	}
}

static int
l_autostart(lua_State *L)
{
	const char *cmd = luaL_checkstring(L, 1);
	char **cmds, *dup;

	if (L != load.L) {
		autostart(cmd);
		return 0;
	}
	/* while loading, wait until the whole file loaded fine */
	if (!(cmds = realloc(load.autostart, (load.nautostart + 1) * sizeof(*cmds))))
		return luaL_error(L, "out of memory");
	load.autostart = cmds;
	if (!(dup = strdup(cmd)))
		return luaL_error(L, "out of memory");
	load.autostart[load.nautostart++] = dup;
	return 0;
}

static int
knownlayout(lua_State *L, const char *name)
{
	Config *c = L == load.L ? &load : &cur;
	int found;

	for (int i = 0; layouts[i]; i++)
		if (!strcmp(name, layouts[i]))
			return 1;
	if (c->layouts == LUA_NOREF)
		return 0;
	lua_rawgeti(L, LUA_REGISTRYINDEX, c->layouts);
	found = lua_getfield(L, -1, name) == LUA_TFUNCTION;
	lua_pop(L, 2);
	return found;
}

static int
l_layout(lua_State *L)
{
	Settings *s = L == load.L ? &load.settings : &settings;
	size_t len;
	const char *name = luaL_checklstring(L, 1, &len);

	if (!knownlayout(L, name) || len >= sizeof(s->layout))
		return luaL_error(L, "unknown layout \"%s\"", name);
	memcpy(s->layout, name, len + 1);
	if (s == &settings)
		arrange();
	return 0;
}

static int
l_addlayout(lua_State *L)
{
	/* fn(n, area, params) returns n boxes { x, y, w, h }; called only when windows
	 * come, go or the layout changes, never per frame */
	Config *c = L == load.L ? &load : &cur;

	luaL_checkstring(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	lua_rawgeti(L, LUA_REGISTRYINDEX, c->layouts);
	lua_pushvalue(L, 1);
	lua_pushvalue(L, 2);
	lua_rawset(L, -3);
	lua_pop(L, 1);
	return 0;
}

static int
l_tiling(lua_State *L)
{
	Settings *s = L == load.L ? &load.settings : &settings;

	s->tiling = lua_isnoneornil(L, 1) ? !s->tiling : lua_toboolean(L, 1);
	if (s == &settings)
		arrange();
	return 0;
}

static int
l_mfact(lua_State *L)
{
	Settings *s = L == load.L ? &load.settings : &settings;
	double f = s->mfact + luaL_checknumber(L, 1);

	s->mfact = f < 0.05 ? 0.05 : f > 0.95 ? 0.95 : f;
	if (s == &settings)
		arrange();
	return 0;
}

static int
l_nmaster(lua_State *L)
{
	Settings *s = L == load.L ? &load.settings : &settings;
	lua_Integer n = s->nmaster + luaL_checkinteger(L, 1);

	s->nmaster = n < 0 ? 0 : n > 20 ? 20 : n;
	if (s == &settings)
		arrange();
	return 0;
}

static int
l_float(lua_State *L)
{
	togglefloat();
	return 0;
}

static int
l_zoom(lua_State *L)
{
	zoom();
	return 0;
}

bool
config_haslayout(const char *name)
{
	bool found;

	if (!cur.L || cur.layouts == LUA_NOREF)
		return false;
	lua_rawgeti(cur.L, LUA_REGISTRYINDEX, cur.layouts);
	found = lua_getfield(cur.L, -1, name) == LUA_TFUNCTION;
	lua_pop(cur.L, 2);
	return found;
}

static int
boxfield(lua_State *L, const char *k, int *v)
{
	int ok;

	lua_getfield(L, -1, k);
	*v = lua_tointegerx(L, -1, &ok);
	lua_pop(L, 1);
	return ok;
}

bool
config_layout(const char *name, struct wlr_box a, int n, struct wlr_box *out)
{
	/* false: no such Lua layout, or it failed; dfwc then uses a built-in one */
	lua_State *L = cur.L;
	int i, ok = 1;

	if (!config_haslayout(name))
		return false;
	lua_rawgeti(L, LUA_REGISTRYINDEX, cur.layouts);
	lua_getfield(L, -1, name);
	lua_pushinteger(L, n);
	lua_createtable(L, 0, 4);
	lua_pushinteger(L, a.x), lua_setfield(L, -2, "x");
	lua_pushinteger(L, a.y), lua_setfield(L, -2, "y");
	lua_pushinteger(L, a.width), lua_setfield(L, -2, "w");
	lua_pushinteger(L, a.height), lua_setfield(L, -2, "h");
	lua_createtable(L, 0, 3);
	lua_pushnumber(L, settings.mfact), lua_setfield(L, -2, "mfact");
	lua_pushinteger(L, settings.nmaster), lua_setfield(L, -2, "nmaster");
	lua_pushinteger(L, settings.gaps_inner), lua_setfield(L, -2, "gap");
	if (lua_pcall(L, 3, 1, 0) != LUA_OK) {
		wlr_log(WLR_ERROR, "config: layout %s: %s", name, lua_tostring(L, -1));
		lua_pop(L, 2);
		return false;
	}
	for (i = 0; ok && i < n; i++) {
		ok = lua_istable(L, -1) && lua_rawgeti(L, -1, i + 1) == LUA_TTABLE
			&& boxfield(L, "x", &out[i].x) && boxfield(L, "y", &out[i].y)
			&& boxfield(L, "w", &out[i].width) && boxfield(L, "h", &out[i].height)
			&& out[i].width > 0 && out[i].height > 0;
		if (lua_istable(L, -1))
			lua_pop(L, 1);
	}
	lua_pop(L, 2);
	if (!ok)
		wlr_log(WLR_ERROR, "config: layout %s: needs %d boxes { x, y, w, h }", name, n);
	return ok;
}

static int
l_style(lua_State *L)
{
	Config *c = L == load.L ? &load : &cur;

	if (!lua_isnil(L, 1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	luaL_unref(L, LUA_REGISTRYINDEX, c->style);
	lua_settop(L, 1);
	c->style = lua_isnil(L, 1) ? LUA_NOREF : luaL_ref(L, LUA_REGISTRYINDEX);
	if (c == &cur)
		applysettings(); /* restyles every window */
	return 0;
}

bool
config_hasstyle(void)
{
	return cur.L && cur.style != LUA_NOREF;
}

void
config_style(const WinInfo *w, Style *st)
{
	/* st holds the settings' look; the function's table overrides parts of it */
	lua_State *L = cur.L;
	const char *err = NULL;
	double d;
	int ok;

	if (!config_hasstyle())
		return;
	lua_rawgeti(L, LUA_REGISTRYINDEX, cur.style);
	lua_createtable(L, 0, 6);
	lua_pushstring(L, w->app_id ? w->app_id : "");
	lua_setfield(L, -2, "app_id");
	lua_pushstring(L, w->title ? w->title : "");
	lua_setfield(L, -2, "title");
	lua_pushboolean(L, w->focused);
	lua_setfield(L, -2, "focused");
	lua_pushboolean(L, w->maximized);
	lua_setfield(L, -2, "maximized");
	lua_pushboolean(L, w->fullscreen);
	lua_setfield(L, -2, "fullscreen");
	lua_pushboolean(L, w->ontop);
	lua_setfield(L, -2, "ontop");
	if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
		wlr_log(WLR_ERROR, "config: style: %s", lua_tostring(L, -1));
		lua_pop(L, 1);
		return;
	}
	if (lua_istable(L, -1)) {
		if (lua_getfield(L, -1, "border") != LUA_TNIL
		&& (lua_type(L, -1) != LUA_TSTRING || !parsecolor(lua_tostring(L, -1), st->border)))
			err = "border";
		lua_pop(L, 1);
		if (lua_getfield(L, -1, "inner_border") != LUA_TNIL
		&& (lua_type(L, -1) != LUA_TSTRING || !parsecolor(lua_tostring(L, -1), st->inner_border)))
			err = "inner_border";
		lua_pop(L, 1);
		if (lua_getfield(L, -1, "opacity") != LUA_TNIL) {
			d = lua_tonumberx(L, -1, &ok);
			if (ok && d >= 0.05 && d <= 1)
				st->opacity = d;
			else
				err = "opacity";
		}
		lua_pop(L, 1);
	} else if (!lua_isnil(L, -1)) {
		err = "the result (should be a table)";
	}
	lua_pop(L, 1);
	if (err)
		wlr_log(WLR_ERROR, "config: style: bad %s", err);
}

static int
l_focus(lua_State *L)
{
	static const char *const dirs[] = { "next", "prev", NULL };

	focusstack(luaL_checkoption(L, 1, NULL, dirs) ? -1 : 1);
	return 0;
}

static int
l_move(lua_State *L)
{
	moveresize(luaL_checkinteger(L, 1), luaL_checkinteger(L, 2), 0, 0);
	return 0;
}

static int
l_resize(lua_State *L)
{
	moveresize(0, 0, luaL_checkinteger(L, 1), luaL_checkinteger(L, 2));
	return 0;
}

static int
l_maximize(lua_State *L)
{
	togglemaximize();
	return 0;
}

static int
l_fullscreen(lua_State *L)
{
	togglefullscreen();
	return 0;
}

static int
l_ontop(lua_State *L)
{
	toggleontop();
	return 0;
}

static int
l_minimize(lua_State *L)
{
	minimize();
	return 0;
}

static int
l_restore(lua_State *L)
{
	restoremenu();
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
	{ "addlayout", l_addlayout },
	{ "autostart", l_autostart },
	{ "bind", l_bind },
	{ "close", l_close },
	{ "float", l_float },
	{ "focus", l_focus },
	{ "fullscreen", l_fullscreen },
	{ "layout", l_layout },
	{ "maximize", l_maximize },
	{ "mfact", l_mfact },
	{ "minimize", l_minimize },
	{ "move", l_move },
	{ "nmaster", l_nmaster },
	{ "ontop", l_ontop },
	{ "quit", l_quit },
	{ "reload", l_reload },
	{ "resize", l_resize },
	{ "restore", l_restore },
	{ "set", l_set },
	{ "spawn", l_spawn },
	{ "style", l_style },
	{ "tiling", l_tiling },
	{ "zoom", l_zoom },
	{ NULL, NULL },
};

static void
freeconfig(Config *c)
{
	if (c->L)
		lua_close(c->L);
	free(c->binds);
	for (size_t i = 0; i < c->nautostart; i++)
		free(c->autostart[i]);
	free(c->autostart);
	*c = (Config){ .style = LUA_NOREF, .layouts = LUA_NOREF };
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
	load = (Config){ .L = L, .style = LUA_NOREF, .settings = defaults };
	lua_newtable(L);
	load.layouts = luaL_ref(L, LUA_REGISTRYINDEX);
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
		configerror(lua_tostring(L, -1));
		freeconfig(&load);
		return 0;
	}
	for (size_t i = 0; i < LENGTH(keep); i++)
		if (!memcmp((char *)&load.settings + keep[i].off, (char *)&cur.settings + keep[i].off, keep[i].size))
			memcpy((char *)&load.settings + keep[i].off, (char *)&settings + keep[i].off, keep[i].size);
	freeconfig(&cur);
	cur = load;
	load = (Config){ .style = LUA_NOREF, .layouts = LUA_NOREF };
	settings = cur.settings;
	configerror(NULL);
	applysettings();
	for (size_t i = 0; i < cur.nautostart; i++)
		autostart(cur.autostart[i]);
	return 1;
}

int
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
		return b->repeat ? BindRepeat : BindDone;
	}
	return BindNone;
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
	for (size_t i = 0; i < nstarted; i++)
		free(started[i]);
	free(started);
}

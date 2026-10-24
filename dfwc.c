/* See LICENSE file for copyright and license details.
 *
 * dfwc is a floating Wayland compositor on wlroots and scenefx. Windows float
 * and keep the stacking order the user gives them; keybinds and settings come
 * from init.lua (see lua.c).
 */
#include <drm_fourcc.h>
#include <errno.h>
#include <fcft/fcft.h>
#include <fcntl.h>
#include <getopt.h>
#include <locale.h>
#include <math.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <uchar.h>
#include <unistd.h>
#include <scenefx/render/fx_renderer/fx_renderer.h>
#include <libinput.h>
#include <scenefx/types/wlr_scene.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/session.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "dfwc.h"

#define MIN(A, B)               ((A) < (B) ? (A) : (B))
#define MAX(A, B)               ((A) > (B) ? (A) : (B))
#define LENGTH(X)               (sizeof(X) / sizeof((X)[0]))
#define LISTEN(E, L, H)         wl_signal_add((E), ((L)->notify = (H), (L)))
#define MODMASK                 (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)

enum { CurNormal, CurMove, CurResize, CurDraw };
/* scene layers, bottom to top */
enum { LyrBg, LyrBottom, LyrClients, LyrOnTop, LyrTop, LyrFullscreen, LyrOverlay, NumLayers };

typedef struct {
	struct wl_list link; /* in clients, most recently focused first */
	struct wlr_xdg_toplevel *toplevel;
	struct wlr_scene_tree *scene;   /* at the window geometry's top-left: borders, then surf */
	struct wlr_scene_tree *surf;    /* the app's surfaces, offset by its geometry */
	struct wlr_scene_rect *ring[2]; /* outer and inner border */
	struct wlr_scene_rect *arm[8];  /* corner brackets, for border_corners */
	struct wlr_xdg_toplevel_decoration_v1 *deco;
	struct wlr_box prev; /* geometry to restore after maximize/fullscreen */
	struct wlr_box geom; /* geometry the borders were last drawn for */
	Style style;         /* last dfwc.style() result, applied on top of settings */
	int maximized, fullscreen, ontop, minimized, radius;
	unsigned int id;     /* names the window in the restore menu */
	struct wl_listener map, unmap, commit, destroy, set_title, set_app_id;
	struct wl_listener request_move, request_resize, request_maximize, request_fullscreen;
	struct wl_listener deco_mode, deco_destroy;
} Client;

typedef struct {
	struct wl_list link;
	struct wlr_keyboard *wlr;
	struct wl_listener modifiers, key, destroy;
} Keyboard;

typedef struct {
	struct wl_list link;
	struct libinput_device *dev;
	struct wl_listener destroy;
} Pointer;

typedef struct {
	struct wl_list link;
	struct wlr_output *wlr;
	struct wlr_box area, usable; /* layout coords; usable leaves out bars' exclusive zones */
	struct wl_list layers[4];    /* LayerSurface, indexed by zwlr_layer_shell_v1_layer */
	struct wl_listener frame, request_state, destroy;
} Monitor;

typedef struct {
	struct wl_list link; /* in Monitor.layers */
	Monitor *mon;
	struct wlr_layer_surface_v1 *wlr;
	struct wlr_scene_layer_surface_v1 *scene;
	struct wl_listener map, unmap, commit, destroy;
} LayerSurface;

typedef struct {
	struct wlr_xdg_popup *wlr;
	struct wl_listener commit, destroy;
} Popup;

typedef struct {
	struct wlr_buffer base;
	pixman_image_t *img;
} TextBuffer; /* CPU-drawn text the scene can show */

uint32_t modkey;
static int nested; /* running in a window inside another desktop */

static struct wl_display *dpy;
static struct wlr_backend *backend;
static struct wlr_session *session; /* NULL when nested */
static struct wlr_renderer *drw;
static struct wlr_allocator *alloc;
static struct wlr_scene *scene;
static struct wlr_scene_tree *layers[NumLayers];
static struct wlr_scene_rect *bg;
static struct wlr_scene_rect *alarmrect; /* full-screen red blink on config errors */
static struct wl_event_source *flashtimer;
static int flashes; /* half-blinks left of the config error flash; odd = red */
static struct wlr_scene_buffer *errbox; /* config error message at the top */
static struct {
	struct wlr_scene_rect *rect;
	struct wl_event_source *timer;
	struct wlr_box box;
	double x, y;  /* where the drag started */
	int pending;  /* drawn and waiting for the next window */
} draw; /* drag-to-open */
static struct {
	int fd;
	struct wl_event_source *src;
	char buf[64];
	size_t len;
} menu = { .fd = -1 }; /* restore menu being read */
static struct fcft_font *font;
static struct wlr_output_layout *output_layout;
static struct wlr_scene_output_layout *scene_layout;
static struct wlr_xdg_shell *xdg_shell;
static struct wlr_layer_shell_v1 *layer_shell;
static struct wlr_xdg_decoration_manager_v1 *deco_mgr;
static struct wlr_xdg_activation_v1 *activation;
static struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
static struct wlr_cursor *cursor;
static struct wlr_xcursor_manager *cursor_mgr;
static struct wlr_seat *seat;
static struct wl_list clients;
static struct wl_list mons;
static struct wl_list keyboards;
static struct wl_list pointers; /* libinput pointers, for touchpad settings */
static LayerSurface *exclusive; /* layer surface holding the keyboard, e.g. wmenu */
static int cycling; /* focus is walking the stack; the order commits when Mod is released */
static struct {
	struct wl_event_source *timer;
	uint32_t mods, keycode;
	xkb_keysym_t sym;
} rep; /* bind being repeated while its key is held */
static struct wl_event_source *sigsrc[2];

/* zwlr_layer_shell_v1_layer -> scene layer */
static const int layermap[] = { LyrBg, LyrBottom, LyrTop, LyrOverlay };

static int cursor_mode = CurNormal;
static Client *grabc;
static struct wlr_box grabgeo; /* grabbed window geometry, layout coords, at grab start */
static double grabx, graby;    /* cursor offset from the grabbed edges */
static uint32_t grabedges;

static void
die(const char *msg)
{
	wlr_log(WLR_ERROR, "%s", msg);
	exit(1);
}

static Client *
clientof(struct wlr_scene_node *node)
{
	/* only a client's root tree carries data */
	for (struct wlr_scene_tree *t = node->parent; t; t = t->node.parent)
		if (t->node.data)
			return t->node.data;
	return NULL;
}

static Client *
clientat(double x, double y, struct wlr_surface **surface, double *sx, double *sy)
{
	struct wlr_scene_node *node = wlr_scene_node_at(&scene->tree.node, x, y, sx, sy);
	struct wlr_scene_surface *ss;

	if (node && node->type == WLR_SCENE_NODE_RECT)
		return clientof(node); /* a border: the window, but no surface to send input to */
	if (!node || node->type != WLR_SCENE_NODE_BUFFER
	|| !(ss = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node))))
		return NULL;
	*surface = ss->surface;
	return clientof(node);
}

static Client *clientfrom(struct wlr_surface *surface);
static void restyle(Client *c);
static void placeerror(void);

static Client *
selclient(void)
{
	return clientfrom(seat->keyboard_state.focused_surface);
}

static Monitor *
selmon(void)
{
	struct wlr_output *o = wlr_output_layout_output_at(output_layout, cursor->x, cursor->y);

	return o ? o->data : NULL;
}

static void
outputbox(struct wlr_box *box)
{
	Monitor *m = selmon();

	*box = m ? m->usable : (struct wlr_box){ 0 };
}

static struct wlr_box
clientbox(Client *c)
{
	struct wlr_box *geo = &c->toplevel->base->geometry;

	return (struct wlr_box){ c->scene->node.x, c->scene->node.y, geo->width, geo->height };
}

static void
resizeclient(Client *c, struct wlr_box b)
{
	/* shortcut: moves the window before the client has resized, so the
	 * left/top edges jitter while resizing from them; wait for the commit */
	wlr_scene_node_set_position(&c->scene->node, b.x, b.y);
	wlr_xdg_toplevel_set_size(c->toplevel, MAX(b.width, 1), MAX(b.height, 1));
}

static void
extents(Client *c, int e[4])
{
	/* how far the borders reach out of the window: top, right, bottom, left */
	const Settings *s = &settings;

	for (int i = 0; i < 4; i++)
		e[i] = c->fullscreen ? 0 : s->border_width[i] + s->inner_border_width + s->border_gap;
}

static struct wlr_box
inset(Client *c, struct wlr_box b)
{
	/* the room left for the window once its borders fit inside b */
	int e[4];

	extents(c, e);
	return (struct wlr_box){ b.x + e[3], b.y + e[0], b.width - e[1] - e[3], b.height - e[0] - e[2] };
}

static struct wlr_box
outset(Client *c, struct wlr_box b)
{
	int e[4];

	extents(c, e);
	return (struct wlr_box){ b.x - e[3], b.y - e[0], b.width + e[1] + e[3], b.height + e[0] + e[2] };
}

static int
clientlayer(Client *c)
{
	return c->fullscreen ? LyrFullscreen : c->ontop ? LyrOnTop : LyrClients;
}

static void
premul(const float in[4], float out[4])
{
	for (int i = 0; i < 3; i++)
		out[i] = in[i] * in[3];
	out[3] = in[3];
}

static void
setring(struct wlr_scene_rect *r, struct wlr_box outer, struct wlr_box hole,
	struct fx_corner_radii radii, int holeradius, const float color[4])
{
	float c[4];

	premul(color, c);
	wlr_scene_node_set_position(&r->node, outer.x, outer.y);
	wlr_scene_rect_set_size(r, outer.width, outer.height);
	wlr_scene_rect_set_corner_radii(r, radii);
	/* the hole is cut out, so a translucent window never shows the border through it */
	wlr_scene_rect_set_clipped_region(r, (struct clipped_region){
		{ hole.x - outer.x, hole.y - outer.y, hole.width, hole.height },
		corner_radii_all(holeradius) });
	wlr_scene_rect_set_color(r, c);
	wlr_scene_node_set_enabled(&r->node, outer.width > 0 && outer.height > 0);
}

static void
stylebuffer(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
	Client *c = data;
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(buffer);

	wlr_scene_buffer_set_opacity(buffer, c->fullscreen ? 1 : c->style.opacity);
	/* round only the main surface; popups and subsurfaces keep their shape */
	if (ss && ss->surface == c->toplevel->base->surface)
		wlr_scene_buffer_set_corner_radius(buffer, c->radius);
}

static void
updateborders(Client *c)
{
	static const float red[4] = { 1, 0.2, 0.2, 1 };
	const Settings *s = &settings;
	struct wlr_box *geo = &c->toplevel->base->geometry, win = { 0, 0, geo->width, geo->height };
	const float *outer = flashes % 2 ? red : c->style.border;
	const float *inner = flashes % 2 ? red : c->style.inner_border;
	const int *w = s->border_width, iw = s->inner_border_width, g = s->border_gap;
	struct wlr_box in, gap, out;
	int bare = c->fullscreen, R, L;

	c->geom = *geo;
	/* the surface draws its geometry at the root's origin */
	wlr_scene_node_set_position(&c->surf->node, -geo->x, -geo->y);
	/* square when maximized/fullscreen, and when the app draws its own shadows */
	c->radius = c->maximized || c->fullscreen || geo->x || geo->y ? 0 : s->corner_radius;
	R = c->radius;

	/* from the window outwards: inner border, gap, outer border */
	in = (struct wlr_box){ -iw, -iw, win.width + 2 * iw, win.height + 2 * iw };
	gap = (struct wlr_box){ in.x - g, in.y - g, in.width + 2 * g, in.height + 2 * g };
	out = (struct wlr_box){ gap.x - w[3], gap.y - w[0],
		gap.width + w[1] + w[3], gap.height + w[0] + w[2] };

	wlr_scene_node_set_enabled(&c->ring[1]->node, !bare && iw);
	if (!bare && iw)
		setring(c->ring[1], in, win, corner_radii_all(R ? R + iw : 0), R, inner);

	wlr_scene_node_set_enabled(&c->ring[0]->node, !bare && !s->border_corners);
	if (!bare && !s->border_corners)
		setring(c->ring[0], out, gap, R ? corner_radii_new(R + iw + g + MAX(w[0], w[3]),
			R + iw + g + MAX(w[0], w[1]), R + iw + g + MAX(w[2], w[1]),
			R + iw + g + MAX(w[2], w[3])) : corner_radii_none(),
			R ? R + iw + g : 0, outer);

	/* corner brackets: a horizontal and a vertical arm at each corner */
	L = MIN(s->border_corners, MIN(out.width, out.height) / 2);
	struct wlr_box arms[8] = {
		{ out.x, out.y, L, w[0] }, { out.x, out.y, w[3], L },
		{ out.x + out.width - L, out.y, L, w[0] }, { out.x + out.width - w[1], out.y, w[1], L },
		{ out.x, out.y + out.height - w[2], L, w[2] }, { out.x, out.y + out.height - L, w[3], L },
		{ out.x + out.width - L, out.y + out.height - w[2], L, w[2] },
		{ out.x + out.width - w[1], out.y + out.height - L, w[1], L },
	};
	for (int i = 0; i < 8; i++) {
		wlr_scene_node_set_enabled(&c->arm[i]->node, !bare && s->border_corners);
		if (!bare && s->border_corners)
			setring(c->arm[i], arms[i], (struct wlr_box){ 0 }, corner_radii_none(), 0, outer);
	}
	wlr_scene_node_for_each_buffer(&c->surf->node, stylebuffer, c);
}

static void
restyle(Client *c)
{
	/* the settings' look for this window, then whatever dfwc.style() says */
	const Settings *s = &settings;
	struct wlr_surface *f = seat->keyboard_state.focused_surface;
	int focused = f == c->toplevel->base->surface;
	WinInfo w = { c->toplevel->app_id, c->toplevel->title, focused, c->maximized, c->fullscreen };

	memcpy(c->style.border, focused ? s->border_focused : s->border_unfocused, sizeof(float[4]));
	memcpy(c->style.inner_border, focused ? s->inner_border_focused : s->inner_border_unfocused,
		sizeof(float[4]));
	c->style.opacity = s->opacity;
	config_style(&w, &c->style);
	updateborders(c);
}

static Client *
clientfrom(struct wlr_surface *surface)
{
	struct wlr_xdg_toplevel *t = surface ? wlr_xdg_toplevel_try_from_wlr_surface(surface) : NULL;

	return t && t->base->data ? ((struct wlr_scene_tree *)t->base->data)->node.data : NULL;
}

static Monitor *
monof(Client *c)
{
	struct wlr_box b = clientbox(c);
	struct wlr_output *o = wlr_output_layout_output_at(output_layout,
		b.x + b.width / 2.0, b.y + b.height / 2.0);

	return o ? o->data : selmon();
}

static void
kbfocus(struct wlr_surface *surface)
{
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	Client *old = selclient(), *c;

	wlr_seat_keyboard_notify_enter(seat, surface,
		kb ? kb->keycodes : NULL, kb ? kb->num_keycodes : 0, kb ? &kb->modifiers : NULL);
	if (old)
		restyle(old);
	if ((c = clientfrom(surface)))
		restyle(c);
}

static void
focusclient(Client *c, int raise)
{
	struct wlr_surface *old = seat->keyboard_state.focused_surface;
	struct wlr_xdg_toplevel *prev;

	if (!c)
		return;
	if (raise)
		wlr_scene_node_raise_to_top(&c->scene->node);
	if (exclusive || old == c->toplevel->base->surface)
		return;
	if (old && (prev = wlr_xdg_toplevel_try_from_wlr_surface(old)))
		wlr_xdg_toplevel_set_activated(prev, false);
	if (!cycling) {
		wl_list_remove(&c->link);
		wl_list_insert(&clients, &c->link);
	}
	wlr_xdg_toplevel_set_activated(c->toplevel, true);
	kbfocus(c->toplevel->base->surface);
}

static void
focustop(void)
{
	Client *c;

	/* clear first so focusclient() doesn't deactivate a dying surface */
	wlr_seat_keyboard_notify_clear_focus(seat);
	/* shortcut: the cleared window isn't restyled; it's unmapping anyway */
	wl_list_for_each(c, &clients, link) {
		if (!c->minimized) {
			focusclient(c, 0);
			return;
		}
	}
}

static void
grab(Client *c, int mode, uint32_t edges)
{
	if (c->fullscreen)
		return;
	if (c->maximized) { /* moving a maximized window keeps its size */
		c->maximized = 0;
		wlr_xdg_toplevel_set_maximized(c->toplevel, false);
	}
	grabc = c;
	cursor_mode = mode;
	grabedges = edges;
	grabgeo = clientbox(c);
	grabx = cursor->x - (edges & WLR_EDGE_RIGHT ? grabgeo.x + grabgeo.width : grabgeo.x);
	graby = cursor->y - (edges & WLR_EDGE_BOTTOM ? grabgeo.y + grabgeo.height : grabgeo.y);
}

static void
ungrab(void)
{
	cursor_mode = CurNormal;
	grabc = NULL;
}

void
focusstack(int dir)
{
	Client *c = selclient();
	struct wl_list *l;

	Client *n;

	if (!c)
		return;
	/* walk most recently focused first, like alt-tab, past minimized windows */
	for (l = &c->link;;) {
		l = dir > 0 ? l->next : l->prev;
		if (l == &clients)
			continue;
		if (l == &c->link)
			return;
		if (!(n = wl_container_of(l, n, link))->minimized)
			break;
	}
	cycling = 1;
	focusclient(n, 1);
}

void
moveresize(int dx, int dy, int dw, int dh)
{
	Client *c = selclient();
	struct wlr_box b;

	if (!c || c->fullscreen)
		return;
	if (c->maximized) {
		c->maximized = 0;
		wlr_xdg_toplevel_set_maximized(c->toplevel, false);
	}
	b = clientbox(c);
	resizeclient(c, (struct wlr_box){ b.x + dx, b.y + dy, b.width + dw, b.height + dh });
}

static void
setmaximized(Client *c, int on)
{
	Monitor *m = monof(c);

	if (on != c->maximized && !c->fullscreen && m) {
		if (on)
			c->prev = clientbox(c);
		c->maximized = on;
		resizeclient(c, on ? inset(c, m->usable) : c->prev);
		restyle(c);
	}
	wlr_xdg_toplevel_set_maximized(c->toplevel, c->maximized);
}

static void
setfullscreen(Client *c, int on)
{
	Monitor *m = monof(c);

	if (on != c->fullscreen && m) {
		if (on && !c->maximized)
			c->prev = clientbox(c);
		/* above bars, below overlays like lock screens */
		c->fullscreen = on;
		wlr_scene_node_reparent(&c->scene->node, layers[clientlayer(c)]);
		resizeclient(c, on ? m->area : c->maximized ? inset(c, m->usable) : c->prev);
		restyle(c);
	}
	wlr_xdg_toplevel_set_fullscreen(c->toplevel, c->fullscreen);
}

void
toggleontop(void)
{
	Client *c = selclient();

	if (!c)
		return;
	c->ontop = !c->ontop;
	wlr_scene_node_reparent(&c->scene->node, layers[clientlayer(c)]);
}

void
minimize(void)
{
	Client *c = selclient();

	if (!c)
		return;
	c->minimized = 1;
	wlr_scene_node_set_enabled(&c->scene->node, false);
	focustop();
}

static int
menuread(int fd, uint32_t mask, void *data)
{
	/* the menu prints the picked line, "id title"; EOF means it closed */
	ssize_t n = 0;
	unsigned int id;
	Client *c;

	if ((mask & WL_EVENT_READABLE) && menu.len < sizeof(menu.buf) - 1
	&& (n = read(fd, menu.buf + menu.len, sizeof(menu.buf) - 1 - menu.len)) > 0) {
		menu.len += n;
		return 0;
	}
	if (n < 0 && errno == EAGAIN)
		return 0;
	menu.buf[menu.len] = '\0';
	if (sscanf(menu.buf, "%u", &id) == 1)
		wl_list_for_each(c, &clients, link)
			if (c->id == id && c->minimized) {
				c->minimized = 0;
				wlr_scene_node_set_enabled(&c->scene->node, true);
				focusclient(c, 1);
				break;
			}
	wl_event_source_remove(menu.src);
	close(menu.fd);
	menu.fd = -1;
	return 0;
}

void
restoremenu(void)
{
	/* list minimized windows in a menu; the one picked comes back */
	int in[2], out[2];
	char line[512];
	pid_t pid;
	Client *c;
	int n;

	if (menu.fd >= 0)
		return;
	/* a socket for the list, so writing to a menu that died is EPIPE, not SIGPIPE */
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, in) < 0)
		return;
	if (pipe2(out, O_CLOEXEC | O_NONBLOCK) < 0) {
		close(in[0]);
		close(in[1]);
		return;
	}
	if ((pid = fork()) == 0) {
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		setsid();
		if (fork() == 0) {
			dup2(in[1], STDIN_FILENO);
			dup2(out[1], STDOUT_FILENO);
			execl("/bin/sh", "/bin/sh", "-c", settings.restore_menu, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	close(in[1]);
	close(out[1]);
	if (pid > 0)
		waitpid(pid, NULL, 0);
	wl_list_for_each(c, &clients, link) {
		if (!c->minimized)
			continue;
		n = snprintf(line, sizeof(line), "%u %s\n", c->id,
			c->toplevel->title ? c->toplevel->title : c->toplevel->app_id ? c->toplevel->app_id : "");
		send(in[0], line, MIN(n, (int)sizeof(line) - 1), MSG_NOSIGNAL | MSG_DONTWAIT);
	}
	close(in[0]);
	menu.fd = out[0];
	menu.len = 0;
	menu.src = wl_event_loop_add_fd(wl_display_get_event_loop(dpy), out[0],
		WL_EVENT_READABLE, menuread, NULL);
}

void
togglemaximize(void)
{
	Client *c = selclient();

	if (c)
		setmaximized(c, !c->maximized);
}

void
togglefullscreen(void)
{
	Client *c = selclient();

	if (c)
		setfullscreen(c, !c->fullscreen);
}

void
killclient(void)
{
	Client *c = selclient();

	if (c)
		wlr_xdg_toplevel_send_close(c->toplevel);
}

void
quit(void)
{
	wl_display_terminate(dpy);
}

void
spawn(const char *cmd)
{
	/* double fork: the child is reparented to init, so we never reap it and
	 * never steal exit statuses from Lua's os.execute/io.popen */
	pid_t pid = fork();
	sigset_t none;

	if (pid == 0) {
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL); /* the event loop blocks SIGINT/SIGTERM */
		setsid();
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	if (pid < 0)
		wlr_log_errno(WLR_ERROR, "fork");
	else
		waitpid(pid, NULL, 0);
}

static int
safetykey(uint32_t mods, xkb_keysym_t sym)
{
	/* hardcoded so a broken config can never lock the user in */
	if (mods != (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT))
		return 0;
	if (sym == XKB_KEY_BackSpace) {
		quit();
		return 1;
	}
	if (session && sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12) {
		wlr_session_change_vt(session, sym - XKB_KEY_F1 + 1);
		return 1;
	}
	return 0;
}

static void
endbox(void)
{
	draw.pending = 0;
	wlr_scene_node_set_enabled(&draw.rect->node, false);
	wl_event_source_timer_update(draw.timer, 0);
}

static int
boxtimeout(void *data)
{
	endbox();
	return 0;
}

static void
showbox(void)
{
	struct wlr_box b = draw.box;

	setring(draw.rect, b, (struct wlr_box){ b.x + 2, b.y + 2, b.width - 4, b.height - 4 },
		corner_radii_none(), 0, settings.border_focused);
}

static int
tinybox(void)
{
	/* a click more than a drag: the app opens at its own size */
	return draw.box.width < 32 || draw.box.height < 32;
}

static void
snap(Client *c, int *x, int *y)
{
	/* pull the window's outer edges onto nearby screen edges and window edges */
	struct wlr_box *geo = &c->toplevel->base->geometry;
	struct wlr_box me = outset(c, (struct wlr_box){ *x, *y, geo->width, geo->height }), o;
	Monitor *m = selmon();
	int d = settings.snap, bx = d + 1, by = d + 1;
	Client *t;

#define TRY(best, v) do { if (abs(v) <= d && abs(v) < abs(best)) best = (v); } while (0)
	if (!d)
		return;
	if (m) {
		o = m->usable;
		TRY(bx, o.x - me.x);
		TRY(bx, o.x + o.width - me.x - me.width);
		TRY(by, o.y - me.y);
		TRY(by, o.y + o.height - me.y - me.height);
	}
	wl_list_for_each(t, &clients, link) {
		if (t == c || t->minimized)
			continue;
		o = outset(t, clientbox(t));
		if (me.y < o.y + o.height && o.y < me.y + me.height) { /* side by side */
			TRY(bx, o.x + o.width - me.x);
			TRY(bx, o.x - me.x - me.width);
		}
		if (me.x < o.x + o.width && o.x < me.x + me.width) { /* stacked */
			TRY(by, o.y + o.height - me.y);
			TRY(by, o.y - me.y - me.height);
		}
	}
#undef TRY
	if (bx <= d)
		*x += bx;
	if (by <= d)
		*y += by;
}

/* input */

static void
keypress(struct wl_listener *listener, void *data)
{
	Keyboard *kb = wl_container_of(listener, kb, key);
	struct wlr_keyboard_key_event *event = data;
	uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr) & MODMASK;
	xkb_keycode_t code = event->keycode + 8;
	const xkb_keysym_t *syms;
	int n, handled;

	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED && cursor_mode == CurDraw
	&& xkb_state_key_get_one_sym(kb->wlr->xkb_state, code) == XKB_KEY_Escape) {
		cursor_mode = CurNormal;
		endbox();
		return;
	}
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		wl_event_source_timer_update(rep.timer, 0); /* a new key ends any repeat */
		/* level 0 so "Mod+Shift+c" matches c, not C, on every layout */
		n = xkb_keymap_key_get_syms_by_level(kb->wlr->keymap, code,
			xkb_state_key_get_layout(kb->wlr->xkb_state, code), 0, &syms);
		for (int i = 0; i < n; i++) {
			if (safetykey(mods, syms[i]))
				return;
			if (!(handled = config_key(mods, syms[i])))
				continue;
			if (handled == BindRepeat && kb->wlr->repeat_info.rate > 0) {
				rep.mods = mods;
				rep.sym = syms[i];
				rep.keycode = event->keycode;
				wl_event_source_timer_update(rep.timer, kb->wlr->repeat_info.delay);
			}
			return;
		}
	} else if (event->keycode == rep.keycode) {
		wl_event_source_timer_update(rep.timer, 0);
	}
	wlr_seat_set_keyboard(seat, kb->wlr);
	wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
}

static int
repeatkey(void *data)
{
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);

	if (kb && kb->repeat_info.rate > 0 && config_key(rep.mods, rep.sym))
		wl_event_source_timer_update(rep.timer, 1000 / kb->repeat_info.rate);
	return 0;
}

static void
keymods(struct wl_listener *listener, void *data)
{
	Keyboard *kb = wl_container_of(listener, kb, modifiers);
	Client *c;

	if (cycling && !(wlr_keyboard_get_modifiers(kb->wlr) & modkey)) {
		/* Mod released: the window cycled to becomes the most recent */
		cycling = 0;
		if ((c = selclient())) {
			wl_list_remove(&c->link);
			wl_list_insert(&clients, &c->link);
		}
	}
	wlr_seat_set_keyboard(seat, kb->wlr);
	wlr_seat_keyboard_notify_modifiers(seat, &kb->wlr->modifiers);
}

static void
destroykeyboard(struct wl_listener *listener, void *data)
{
	Keyboard *kb = wl_container_of(listener, kb, destroy);

	wl_list_remove(&kb->modifiers.link);
	wl_list_remove(&kb->key.link);
	wl_list_remove(&kb->destroy.link);
	wl_list_remove(&kb->link);
	free(kb);
}

static struct xkb_keymap *
keymap(void)
{
	/* empty settings fall back to XKB_DEFAULT_*, a bad layout to us */
	struct xkb_rule_names names = {
		.layout = *settings.xkb_layout ? settings.xkb_layout : NULL,
		.variant = *settings.xkb_variant ? settings.xkb_variant : NULL,
		.options = *settings.xkb_options ? settings.xkb_options : NULL,
		.model = *settings.xkb_model ? settings.xkb_model : NULL,
	};
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);

	if (!km) {
		wlr_log(WLR_ERROR, "invalid keyboard layout, falling back to us");
		km = xkb_keymap_new_from_names(ctx, &(struct xkb_rule_names){ .layout = "us" },
			XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
	xkb_context_unref(ctx);
	return km;
}

static void
createkeyboard(struct wlr_keyboard *wlr)
{
	Keyboard *kb = calloc(1, sizeof(*kb));
	struct xkb_keymap *km = keymap();

	if (!kb || !km) {
		wlr_log(WLR_ERROR, "couldn't set up keyboard %s", wlr->base.name);
		free(kb);
		xkb_keymap_unref(km);
		return;
	}
	kb->wlr = wlr;
	wlr_keyboard_set_keymap(wlr, km);
	xkb_keymap_unref(km);
	wlr_keyboard_set_repeat_info(wlr, settings.repeat_rate, settings.repeat_delay);
	LISTEN(&wlr->events.modifiers, &kb->modifiers, keymods);
	LISTEN(&wlr->events.key, &kb->key, keypress);
	LISTEN(&wlr->base.events.destroy, &kb->destroy, destroykeyboard);
	wl_list_insert(&keyboards, &kb->link);
	wlr_seat_set_keyboard(seat, wlr);
}

static void
configpointer(struct libinput_device *dev)
{
	/* only touchpads: they're the devices that can tap */
	if (!libinput_device_config_tap_get_finger_count(dev))
		return;
	libinput_device_config_tap_set_enabled(dev, settings.touchpad_tap
		? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
	if (libinput_device_config_scroll_has_natural_scroll(dev))
		libinput_device_config_scroll_set_natural_scroll_enabled(dev, settings.touchpad_natural_scroll);
	if (libinput_device_config_dwt_is_available(dev))
		libinput_device_config_dwt_set_enabled(dev, settings.touchpad_dwt
			? LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED);
	if (libinput_device_config_accel_is_available(dev))
		libinput_device_config_accel_set_speed(dev, settings.touchpad_speed);
}

static void
destroypointer(struct wl_listener *listener, void *data)
{
	Pointer *p = wl_container_of(listener, p, destroy);

	wl_list_remove(&p->destroy.link);
	wl_list_remove(&p->link);
	free(p);
}

static void
createpointer(struct wlr_input_device *dev)
{
	Pointer *p;

	wlr_cursor_attach_input_device(cursor, dev);
	if (!wlr_input_device_is_libinput(dev) || !(p = calloc(1, sizeof(*p))))
		return;
	p->dev = wlr_libinput_get_device_handle(dev);
	configpointer(p->dev);
	LISTEN(&dev->events.destroy, &p->destroy, destroypointer);
	wl_list_insert(&pointers, &p->link);
}

static void
inputdevice(struct wl_listener *listener, void *data)
{
	struct wlr_input_device *dev = data;

	if (dev->type == WLR_INPUT_DEVICE_KEYBOARD)
		createkeyboard(wlr_keyboard_from_input_device(dev));
	else if (dev->type == WLR_INPUT_DEVICE_POINTER)
		createpointer(dev);
}

static void
motionnotify(uint32_t time)
{
	struct wlr_surface *surface = NULL;
	double sx, sy;
	Client *c;
	int l, r, t, b, ex, ey, x, y;

	if (cursor_mode == CurMove) {
		x = cursor->x - grabx;
		y = cursor->y - graby;
		snap(grabc, &x, &y);
		wlr_scene_node_set_position(&grabc->scene->node, x, y);
		return;
	}
	if (cursor_mode == CurDraw) {
		draw.box = (struct wlr_box){ MIN(draw.x, cursor->x), MIN(draw.y, cursor->y),
			fabs(cursor->x - draw.x), fabs(cursor->y - draw.y) };
		showbox();
		return;
	}
	if (cursor_mode == CurResize) {
		l = grabgeo.x, r = grabgeo.x + grabgeo.width;
		t = grabgeo.y, b = grabgeo.y + grabgeo.height;
		ex = cursor->x - grabx, ey = cursor->y - graby;
		if (grabedges & WLR_EDGE_LEFT)
			l = MIN(ex, r - 1);
		else if (grabedges & WLR_EDGE_RIGHT)
			r = MAX(ex, l + 1);
		if (grabedges & WLR_EDGE_TOP)
			t = MIN(ey, b - 1);
		else if (grabedges & WLR_EDGE_BOTTOM)
			b = MAX(ey, t + 1);
		resizeclient(grabc, (struct wlr_box){ l, t, r - l, b - t });
		return;
	}

	if ((c = clientat(cursor->x, cursor->y, &surface, &sx, &sy)))
		focusclient(c, 0); /* focus follows mouse, only a click raises */
	if (!surface) {
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
		wlr_seat_pointer_clear_focus(seat);
		return;
	}
	wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(seat, time, sx, sy);
}

static void
motionrelative(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_motion_event *event = data;

	wlr_cursor_move(cursor, &event->pointer->base, event->delta_x, event->delta_y);
	motionnotify(event->time_msec);
}

static void
motionabsolute(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_motion_absolute_event *event = data;

	wlr_cursor_warp_absolute(cursor, &event->pointer->base, event->x, event->y);
	motionnotify(event->time_msec);
}

static void
buttonpress(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_button_event *event = data;
	struct wlr_surface *surface = NULL;
	struct wlr_layer_surface_v1 *ls;
	struct wlr_keyboard *kb;
	struct wlr_box box;
	double sx, sy, cx, cy;
	Client *c;

	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		c = clientat(cursor->x, cursor->y, &surface, &sx, &sy);
		focusclient(c, 1);
		if (!c && surface && !exclusive && (ls = wlr_layer_surface_v1_try_from_wlr_surface(
				wlr_surface_get_root_surface(surface))) && ls->current.keyboard_interactive)
			kbfocus(ls->surface); /* on-demand layer surfaces get the keyboard on click */
		kb = wlr_seat_get_keyboard(seat);
		if (c && kb && (wlr_keyboard_get_modifiers(kb) & MODMASK) == modkey
		&& (event->button == BTN_LEFT || event->button == BTN_RIGHT)) {
			/* Mod+left moves, Mod+right resizes from the nearest corner */
			box = clientbox(c);
			cx = box.x + box.width / 2.0;
			cy = box.y + box.height / 2.0;
			if (event->button == BTN_LEFT)
				grab(c, CurMove, WLR_EDGE_NONE);
			else
				grab(c, CurResize, (cursor->x < cx ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT)
					| (cursor->y < cy ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM));
			return;
		}
		if (!c && kb && (wlr_keyboard_get_modifiers(kb) & MODMASK) == modkey
		&& event->button == BTN_LEFT) {
			/* Mod+drag on the desktop draws where the next window goes */
			endbox();
			cursor_mode = CurDraw;
			draw.x = cursor->x;
			draw.y = cursor->y;
			draw.box = (struct wlr_box){ cursor->x, cursor->y, 0, 0 };
			return;
		}
	} else if (cursor_mode == CurDraw) {
		cursor_mode = CurNormal;
		draw.pending = 1;
		if (tinybox())
			wlr_scene_node_set_enabled(&draw.rect->node, false);
		wl_event_source_timer_update(draw.timer, settings.box_timeout);
		spawn(settings.launcher);
		return;
	} else if (cursor_mode != CurNormal) {
		ungrab();
	}
	wlr_seat_pointer_notify_button(seat, event->time_msec, event->button, event->state);
}

static void
axisnotify(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_axis_event *event = data;

	wlr_seat_pointer_notify_axis(seat, event->time_msec, event->orientation, event->delta,
		event->delta_discrete, event->source, event->relative_direction);
}

static void
framenotify(struct wl_listener *listener, void *data)
{
	wlr_seat_pointer_notify_frame(seat);
}

static void
setcursor(struct wl_listener *listener, void *data)
{
	struct wlr_seat_pointer_request_set_cursor_event *event = data;

	/* only the client under the pointer may set its image */
	if (cursor_mode == CurNormal && event->seat_client == seat->pointer_state.focused_client)
		wlr_cursor_set_surface(cursor, event->surface, event->hotspot_x, event->hotspot_y);
}

static void
setcursorshape(struct wl_listener *listener, void *data)
{
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;

	if (cursor_mode == CurNormal && event->seat_client == seat->pointer_state.focused_client)
		wlr_cursor_set_xcursor(cursor, cursor_mgr, wlr_cursor_shape_v1_name(event->shape));
}

static void
setsel(struct wl_listener *listener, void *data)
{
	struct wlr_seat_request_set_selection_event *event = data;

	wlr_seat_set_selection(seat, event->source, event->serial);
}

static void
setpsel(struct wl_listener *listener, void *data)
{
	struct wlr_seat_request_set_primary_selection_event *event = data;

	wlr_seat_set_primary_selection(seat, event->source, event->serial);
}

/* outputs */

static void
rendermon(struct wl_listener *listener, void *data)
{
	Monitor *m = wl_container_of(listener, m, frame);
	struct wlr_scene_output *so = wlr_scene_get_scene_output(scene, m->wlr);
	struct timespec now;

	wlr_scene_output_commit(so, NULL);
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(so, &now);
}

static void
requestmonstate(struct wl_listener *listener, void *data)
{
	const struct wlr_output_event_request_state *event = data;
	Monitor *m = wl_container_of(listener, m, request_state);

	wlr_output_commit_state(m->wlr, event->state);
}

static void
destroymon(struct wl_listener *listener, void *data)
{
	Monitor *m = wl_container_of(listener, m, destroy);
	LayerSurface *l, *tmp;

	for (size_t i = 0; i < LENGTH(m->layers); i++)
		wl_list_for_each_safe(l, tmp, &m->layers[i], link)
			wlr_layer_surface_v1_destroy(l->wlr);
	wl_list_remove(&m->link);
	wl_list_remove(&m->frame.link);
	wl_list_remove(&m->request_state.link);
	wl_list_remove(&m->destroy.link);
	free(m);
}

static void
arrangelayers(Monitor *m)
{
	struct wlr_box usable = m->area;
	LayerSurface *l;
	Client *c;

	/* exclusive zones (bars) first, so the rest fits in what they leave */
	for (int excl = 1; excl >= 0; excl--)
		for (int i = LENGTH(m->layers) - 1; i >= 0; i--)
			wl_list_for_each(l, &m->layers[i], link)
				if (l->wlr->initialized && (l->wlr->current.exclusive_zone > 0) == excl)
					wlr_scene_layer_surface_v1_configure(l->scene, &m->area, &usable);
	m->usable = usable;
	wl_list_for_each(c, &clients, link)
		if (monof(c) == m && (c->maximized || c->fullscreen))
			resizeclient(c, c->fullscreen ? m->area : inset(c, m->usable));
}

static void
updatemons(struct wl_listener *listener, void *data)
{
	struct wlr_box box;
	Monitor *m;

	wlr_output_layout_get_box(output_layout, NULL, &box);
	wlr_scene_node_set_position(&bg->node, box.x, box.y);
	wlr_scene_rect_set_size(bg, box.width, box.height);
	wlr_scene_node_set_position(&alarmrect->node, box.x, box.y);
	wlr_scene_rect_set_size(alarmrect, box.width, box.height);
	wl_list_for_each(m, &mons, link) {
		wlr_output_layout_get_box(output_layout, m->wlr, &m->area);
		arrangelayers(m);
	}
	placeerror();
}

static void
createmon(struct wl_listener *listener, void *data)
{
	struct wlr_output *wlr = data;
	struct wlr_output_state state;
	struct wlr_output_mode *mode;
	Monitor *m;

	if (!wlr_output_init_render(wlr, alloc, drw) || !(m = calloc(1, sizeof(*m)))) {
		wlr_log(WLR_ERROR, "couldn't set up output %s", wlr->name);
		return;
	}
	m->wlr = wlr;
	wlr->data = m;
	for (size_t i = 0; i < LENGTH(m->layers); i++)
		wl_list_init(&m->layers[i]);
	wl_list_insert(&mons, &m->link);
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	if ((mode = wlr_output_preferred_mode(wlr)))
		wlr_output_state_set_mode(&state, mode);
	wlr_output_state_set_scale(&state, settings.scale);
	wlr_output_commit_state(wlr, &state);
	wlr_output_state_finish(&state);

	LISTEN(&wlr->events.frame, &m->frame, rendermon);
	LISTEN(&wlr->events.request_state, &m->request_state, requestmonstate);
	LISTEN(&wlr->events.destroy, &m->destroy, destroymon);
	/* a new screen extends the desktop to the right; windows stay put */
	wlr_scene_output_layout_add_output(scene_layout,
		wlr_output_layout_add_auto(output_layout, wlr), wlr_scene_output_create(scene, wlr));
}

/* windows */

static void
mapnotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, map), *p;
	struct wlr_box *geo = &c->toplevel->base->geometry, area, around;
	int w, h;

	static unsigned int ids;

	c->id = ++ids;
	wl_list_insert(&clients, &c->link);
	wlr_scene_node_set_enabled(&c->scene->node, true);
	outputbox(&area);
	area = inset(c, area);
	if (draw.pending && !c->toplevel->parent) {
		/* the box drawn for it: its corner if it was a click, else its size too */
		if (tinybox())
			wlr_scene_node_set_position(&c->scene->node, draw.box.x, draw.box.y);
		else
			resizeclient(c, inset(c, draw.box));
		endbox();
	} else if (!wlr_box_empty(&area)) {
		/* center on the screen, dialogs on their parent; shrink to fit */
		around = area;
		if (c->toplevel->parent && (p = clientfrom(c->toplevel->parent->base->surface)))
			around = clientbox(p);
		w = MIN(geo->width, area.width);
		h = MIN(geo->height, area.height);
		if (w != geo->width || h != geo->height)
			wlr_xdg_toplevel_set_size(c->toplevel, w, h);
		wlr_scene_node_set_position(&c->scene->node,
			MIN(MAX(around.x + (around.width - w) / 2, area.x), area.x + area.width - w),
			MIN(MAX(around.y + (around.height - h) / 2, area.y), area.y + area.height - h));
	}
	restyle(c);
	focusclient(c, 1);
	if (c->toplevel->requested.fullscreen)
		setfullscreen(c, 1);
	else if (c->toplevel->requested.maximized)
		setmaximized(c, 1);
}

static void
unmapnotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, unmap);
	int focused = selclient() == c;

	/* an app that hides and shows its window starts over as a plain one */
	c->maximized = c->fullscreen = c->minimized = 0;
	wlr_scene_node_reparent(&c->scene->node, layers[clientlayer(c)]);
	wlr_scene_node_set_enabled(&c->scene->node, false);
	if (c == grabc)
		ungrab();
	wl_list_remove(&c->link);
	if (focused)
		focustop();
}

static void
commitnotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, commit);
	struct wlr_box *geo = &c->toplevel->base->geometry;

	if (c->toplevel->base->surface->mapped) {
		if (!wlr_box_equal(geo, &c->geom))
			updateborders(c);
		else if (c->radius || c->style.opacity < 1)
			wlr_scene_node_for_each_buffer(&c->surf->node, stylebuffer, c); /* new subsurfaces */
		return;
	}
	if (!c->toplevel->base->initial_commit)
		return;
	/* 0x0 lets the client pick its own size */
	wlr_xdg_toplevel_set_size(c->toplevel, 0, 0);
	if (c->deco)
		wlr_xdg_toplevel_decoration_v1_set_mode(c->deco,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void
requestmove(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, request_move);

	grab(c, CurMove, WLR_EDGE_NONE);
}

static void
requestresize(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, request_resize);
	struct wlr_xdg_toplevel_resize_event *event = data;

	grab(c, CurResize, event->edges);
}

static void
requestmaximize(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, request_maximize);

	/* before mapping mapnotify applies it; xdg-shell still wants a reply */
	if (c->toplevel->base->surface->mapped)
		setmaximized(c, c->toplevel->requested.maximized);
	else if (c->toplevel->base->initialized)
		wlr_xdg_surface_schedule_configure(c->toplevel->base);
}

static void
requestfullscreen(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, request_fullscreen);

	if (c->toplevel->base->surface->mapped)
		setfullscreen(c, c->toplevel->requested.fullscreen);
	else if (c->toplevel->base->initialized)
		wlr_xdg_surface_schedule_configure(c->toplevel->base);
}

static void
destroynotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, destroy);

	wl_list_remove(&c->map.link);
	wl_list_remove(&c->unmap.link);
	wl_list_remove(&c->commit.link);
	wl_list_remove(&c->destroy.link);
	wl_list_remove(&c->request_move.link);
	wl_list_remove(&c->request_resize.link);
	wl_list_remove(&c->request_maximize.link);
	wl_list_remove(&c->request_fullscreen.link);
	wl_list_remove(&c->set_title.link);
	wl_list_remove(&c->set_app_id.link);
	wlr_scene_node_destroy(&c->scene->node);
	if (c->deco) { /* its destroy fires after ours, when c is gone */
		wl_list_remove(&c->deco_mode.link);
		wl_list_remove(&c->deco_destroy.link);
	}
	free(c);
}

static void
retitle(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_title);

	if (c->toplevel->base->surface->mapped && config_hasstyle())
		restyle(c);
}

static void
reappid(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_app_id);

	if (c->toplevel->base->surface->mapped && config_hasstyle())
		restyle(c);
}

static void
requestdecomode(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, deco_mode);

	/* dfwc draws the borders; apps that insist keep their own decorations */
	if (c->toplevel->base->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(c->deco,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void
destroydeco(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, deco_destroy);

	wl_list_remove(&c->deco_mode.link);
	wl_list_remove(&c->deco_destroy.link);
	c->deco = NULL;
}

static void
createdeco(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_toplevel_decoration_v1 *deco = data;
	Client *c = clientfrom(deco->toplevel->base->surface);

	c->deco = deco;
	LISTEN(&deco->events.request_mode, &c->deco_mode, requestdecomode);
	LISTEN(&deco->events.destroy, &c->deco_destroy, destroydeco);
	requestdecomode(&c->deco_mode, deco);
}

static void
activate(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	Client *c = clientfrom(event->surface);

	if (c && c->toplevel->base->surface->mapped)
		focusclient(c, 1);
}

static void
createnotify(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_toplevel *toplevel = data;
	Client *c = calloc(1, sizeof(*c));

	if (!c) {
		wl_resource_post_no_memory(toplevel->resource);
		return;
	}
	c->toplevel = toplevel;
	c->scene = wlr_scene_tree_create(layers[LyrClients]);
	wlr_scene_node_set_enabled(&c->scene->node, false); /* until mapped */
	/* created first, so they sit below the surface */
	for (size_t i = 0; i < LENGTH(c->ring); i++)
		c->ring[i] = wlr_scene_rect_create(c->scene, 0, 0, (float[4]){ 0 });
	for (size_t i = 0; i < LENGTH(c->arm); i++)
		c->arm[i] = wlr_scene_rect_create(c->scene, 0, 0, (float[4]){ 0 });
	c->surf = wlr_scene_xdg_surface_create(c->scene, toplevel->base);
	c->scene->node.data = c->surf->node.data = c;
	toplevel->base->data = c->surf;

	LISTEN(&toplevel->base->surface->events.map, &c->map, mapnotify);
	LISTEN(&toplevel->base->surface->events.unmap, &c->unmap, unmapnotify);
	LISTEN(&toplevel->base->surface->events.commit, &c->commit, commitnotify);
	LISTEN(&toplevel->events.destroy, &c->destroy, destroynotify);
	LISTEN(&toplevel->events.request_move, &c->request_move, requestmove);
	LISTEN(&toplevel->events.request_resize, &c->request_resize, requestresize);
	LISTEN(&toplevel->events.request_maximize, &c->request_maximize, requestmaximize);
	LISTEN(&toplevel->events.request_fullscreen, &c->request_fullscreen, requestfullscreen);
	LISTEN(&toplevel->events.set_title, &c->set_title, retitle);
	LISTEN(&toplevel->events.set_app_id, &c->set_app_id, reappid);
}

static void
commitpopup(struct wl_listener *listener, void *data)
{
	Popup *p = wl_container_of(listener, p, commit);
	struct wlr_scene_tree *tree = p->wlr->base->data;
	Client *c;
	struct wlr_box box;

	if (!p->wlr->base->initial_commit)
		return;
	/* keep menus on screen; the box is relative to the toplevel's surface */
	if ((c = clientof(&tree->node))) {
		outputbox(&box);
		box.x -= c->scene->node.x - c->toplevel->base->geometry.x;
		box.y -= c->scene->node.y - c->toplevel->base->geometry.y;
		wlr_xdg_popup_unconstrain_from_box(p->wlr, &box);
	}
	wlr_xdg_surface_schedule_configure(p->wlr->base);
}

static void
destroypopup(struct wl_listener *listener, void *data)
{
	Popup *p = wl_container_of(listener, p, destroy);

	wl_list_remove(&p->commit.link);
	wl_list_remove(&p->destroy.link);
	free(p);
}

static void
createpopup(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_popup *popup = data;
	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(popup->parent);
	Popup *p;

	if (!parent || !parent->data)
		return;
	if (!(p = calloc(1, sizeof(*p)))) {
		wl_resource_post_no_memory(popup->resource);
		return;
	}
	p->wlr = popup;
	popup->base->data = wlr_scene_xdg_surface_create(parent->data, popup->base);
	LISTEN(&popup->base->surface->events.commit, &p->commit, commitpopup);
	LISTEN(&popup->events.destroy, &p->destroy, destroypopup);
}

/* layer surfaces: wallpapers, bars, launchers */

static void
maplayer(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, map);

	if (l->wlr->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
		exclusive = l;
		kbfocus(l->wlr->surface);
	}
	arrangelayers(l->mon);
}

static void
unmaplayer(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, unmap);

	if (exclusive == l)
		exclusive = NULL;
	if (seat->keyboard_state.focused_surface == l->wlr->surface)
		focustop();
	arrangelayers(l->mon);
}

static void
commitlayer(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, commit);
	struct wlr_layer_surface_v1 *ls = l->wlr;
	struct wlr_layer_surface_v1_state old;

	if (ls->initial_commit) {
		/* the first configure must fit the state the client asked for */
		old = ls->current;
		ls->current = ls->pending;
		arrangelayers(l->mon);
		ls->current = old;
		return;
	}
	if (!ls->current.committed)
		return;
	if (ls->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		wlr_scene_node_reparent(&l->scene->tree->node, layers[layermap[ls->current.layer]]);
		wl_list_remove(&l->link);
		wl_list_insert(&l->mon->layers[ls->current.layer], &l->link);
	}
	arrangelayers(l->mon);
}

static void
destroylayer(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, destroy);

	wl_list_remove(&l->link);
	wl_list_remove(&l->map.link);
	wl_list_remove(&l->unmap.link);
	wl_list_remove(&l->commit.link);
	wl_list_remove(&l->destroy.link);
	free(l);
}

static void
createlayer(struct wl_listener *listener, void *data)
{
	struct wlr_layer_surface_v1 *ls = data;
	LayerSurface *l = NULL;
	Monitor *m;

	if (!ls->output && (m = selmon()))
		ls->output = m->wlr;
	if (!ls->output || !(m = ls->output->data) || !(l = calloc(1, sizeof(*l)))) {
		wlr_layer_surface_v1_destroy(ls);
		return;
	}
	l->wlr = ls;
	l->mon = m;
	ls->data = l;
	/* shortcut: popups of layer surfaces (waybar menus) aren't shown yet */
	l->scene = wlr_scene_layer_surface_v1_create(layers[layermap[ls->pending.layer]], ls);
	wl_list_insert(&m->layers[ls->pending.layer], &l->link);
	LISTEN(&ls->surface->events.map, &l->map, maplayer);
	LISTEN(&ls->surface->events.unmap, &l->unmap, unmaplayer);
	LISTEN(&ls->surface->events.commit, &l->commit, commitlayer);
	LISTEN(&ls->events.destroy, &l->destroy, destroylayer);
}

void
applysettings(void)
{
	static Settings prev = { .cursor_size = -1 }; /* -1 forces the first cursor setup */
	const Settings *s = &settings;
	struct wlr_output_state state;
	struct xkb_keymap *km;
	Keyboard *kb;
	Pointer *p;
	Monitor *m;
	Client *c;
	char size[16];

	modkey = s->modkey ? s->modkey : nested ? WLR_MODIFIER_ALT : WLR_MODIFIER_LOGO;

	/* a new keymap resets held modifiers, so only swap it when it changed */
	km = strcmp(s->xkb_layout, prev.xkb_layout) || strcmp(s->xkb_variant, prev.xkb_variant)
		|| strcmp(s->xkb_options, prev.xkb_options) || strcmp(s->xkb_model, prev.xkb_model)
		? keymap() : NULL;
	wl_list_for_each(kb, &keyboards, link) {
		if (km)
			wlr_keyboard_set_keymap(kb->wlr, km);
		wlr_keyboard_set_repeat_info(kb->wlr, s->repeat_rate, s->repeat_delay);
	}
	xkb_keymap_unref(km);

	wl_list_for_each(p, &pointers, link)
		configpointer(p->dev);

	if (strcmp(s->cursor_theme, prev.cursor_theme) || s->cursor_size != prev.cursor_size) {
		/* exported so clients draw the same cursor */
		if (*s->cursor_theme)
			setenv("XCURSOR_THEME", s->cursor_theme, 1);
		snprintf(size, sizeof(size), "%d", s->cursor_size);
		setenv("XCURSOR_SIZE", size, 1);
		if (cursor_mgr)
			wlr_xcursor_manager_destroy(cursor_mgr);
		cursor_mgr = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"), s->cursor_size);
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
	}

	wlr_scene_rect_set_color(bg, (float[4]){ s->background[0] * s->background[3],
		s->background[1] * s->background[3], s->background[2] * s->background[3], s->background[3] });

	wl_list_for_each(m, &mons, link) {
		if (m->wlr->scale != (float)s->scale) {
			wlr_output_state_init(&state);
			wlr_output_state_set_scale(&state, s->scale);
			wlr_output_commit_state(m->wlr, &state);
			wlr_output_state_finish(&state);
		}
		arrangelayers(m); /* border sizes change what maximized windows get */
	}
	wl_list_for_each(c, &clients, link)
		restyle(c);
	prev = *s;
}

static int
flashstep(void *data)
{
	Client *c;

	if (flashes > 0)
		flashes--;
	wl_list_for_each(c, &clients, link)
		updateborders(c);
	wlr_scene_node_set_enabled(&alarmrect->node, settings.error_alarm_fullscreen && flashes % 2);
	if (flashes)
		wl_event_source_timer_update(flashtimer, 150);
	return 0;
}

static void
textbufdestroy(struct wlr_buffer *buffer)
{
	TextBuffer *t = wl_container_of(buffer, t, base);

	pixman_image_unref(t->img);
	free(t);
}

static bool
textbufaccess(struct wlr_buffer *buffer, uint32_t flags, void **data, uint32_t *format, size_t *stride)
{
	TextBuffer *t = wl_container_of(buffer, t, base);

	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
		return false;
	*data = pixman_image_get_data(t->img);
	*format = DRM_FORMAT_ARGB8888;
	*stride = pixman_image_get_stride(t->img);
	return true;
}

static void
textbufend(struct wlr_buffer *buffer)
{
}

static const struct wlr_buffer_impl textbufimpl = {
	.destroy = textbufdestroy,
	.begin_data_ptr_access = textbufaccess,
	.end_data_ptr_access = textbufend,
};

static void
placeerror(void)
{
	/* top center; again when screens appear, since startup errors come before them */
	struct wlr_box area;

	if (!errbox)
		return;
	outputbox(&area);
	wlr_scene_node_set_position(&errbox->node,
		area.x + MAX((area.width - errbox->buffer->width) / 2, 0), area.y + 8);
}

static void
showerror(const char *msg)
{
	/* a black box at the top of the screen, until the config loads fine */
	enum { Pad = 8 };
	static pixman_color_t white = { 0xffff, 0xffff, 0xffff, 0xffff };
	const struct fcft_glyph *glyphs[256];
	pixman_image_t *fg;
	TextBuffer *t;
	struct wlr_box area;
	mbstate_t ps = { 0 };
	char32_t cp;
	size_t n = 0, len;
	int w = 2 * Pad, h, x;

	if (errbox) {
		wlr_scene_node_destroy(&errbox->node);
		errbox = NULL;
	}
	/* shortcut: drawn at scale 1; pass the output's dpi to fcft for crisp HiDPI text */
	if (!msg || (!font && !(font = fcft_from_name(1, (const char *[]){ "monospace:size=11" }, NULL))))
		return;
	outputbox(&area);
	for (const char *p = msg; *p && n < LENGTH(glyphs); p += len) {
		if ((len = mbrtoc32(&cp, p, strlen(p), &ps)) == 0 || len > 4) { /* invalid UTF-8 */
			cp = '?';
			len = 1;
			ps = (mbstate_t){ 0 };
		}
		if (!(glyphs[n] = fcft_rasterize_char_utf32(font, cp, FCFT_SUBPIXEL_NONE)))
			continue;
		if (area.width && w + glyphs[n]->advance.x > area.width)
			break;
		w += glyphs[n++]->advance.x;
	}
	h = font->height + 2 * Pad;
	if (!(t = calloc(1, sizeof(*t)))
	|| !(t->img = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, w, h, NULL, 0))) {
		free(t);
		return;
	}
	/* black at 85%, so the windows behind still show a little */
	pixman_image_fill_rectangles(PIXMAN_OP_SRC, t->img, &(pixman_color_t){ 0, 0, 0, 0xd999 },
		1, &(pixman_rectangle16_t){ 0, 0, w, h });
	fg = pixman_image_create_solid_fill(&white);
	x = Pad;
	for (size_t i = 0; i < n; i++) {
		/* color glyphs (emoji) are images; the rest are masks for the white fill */
		pixman_image_composite32(PIXMAN_OP_OVER, glyphs[i]->is_color_glyph ? glyphs[i]->pix : fg,
			glyphs[i]->is_color_glyph ? NULL : glyphs[i]->pix, t->img, 0, 0, 0, 0,
			x + glyphs[i]->x, Pad + font->ascent - glyphs[i]->y, glyphs[i]->width, glyphs[i]->height);
		x += glyphs[i]->advance.x;
	}
	pixman_image_unref(fg);
	wlr_buffer_init(&t->base, &textbufimpl, w, h);
	errbox = wlr_scene_buffer_create(layers[LyrOverlay], &t->base);
	wlr_buffer_drop(&t->base); /* the scene buffer holds its own reference */
	placeerror();
}

void
configerror(const char *msg)
{
	/* NULL: the config loaded fine. Otherwise three red blinks of every
	 * border (and the whole screen with error_alarm_fullscreen) */
	showerror(msg);
	if (!msg)
		return;
	flashes = 6;
	flashstep(NULL);
}

/* setup */

static struct wl_listener new_output = { .notify = createmon };
static struct wl_listener new_input = { .notify = inputdevice };
static struct wl_listener new_toplevel = { .notify = createnotify };
static struct wl_listener new_popup = { .notify = createpopup };
static struct wl_listener cursor_motion = { .notify = motionrelative };
static struct wl_listener cursor_motion_absolute = { .notify = motionabsolute };
static struct wl_listener cursor_button = { .notify = buttonpress };
static struct wl_listener cursor_axis = { .notify = axisnotify };
static struct wl_listener cursor_frame = { .notify = framenotify };
static struct wl_listener request_cursor = { .notify = setcursor };
static struct wl_listener request_sel = { .notify = setsel };
static struct wl_listener request_psel = { .notify = setpsel };
static struct wl_listener request_shape = { .notify = setcursorshape };
static struct wl_listener layout_change = { .notify = updatemons };
static struct wl_listener new_layer = { .notify = createlayer };
static struct wl_listener new_deco = { .notify = createdeco };
static struct wl_listener request_activate = { .notify = activate };

static int
quitsignal(int sig, void *data)
{
	quit();
	return 0;
}

static void
setup(void)
{
	struct wl_event_loop *loop;

	setlocale(LC_CTYPE, ""); /* error messages are UTF-8 */
	fcft_init(FCFT_LOG_COLORIZE_NEVER, false, FCFT_LOG_CLASS_ERROR);
	dpy = wl_display_create();
	loop = wl_display_get_event_loop(dpy);
	sigsrc[0] = wl_event_loop_add_signal(loop, SIGINT, quitsignal, NULL);
	sigsrc[1] = wl_event_loop_add_signal(loop, SIGTERM, quitsignal, NULL);
	rep.timer = wl_event_loop_add_timer(loop, repeatkey, NULL);
	flashtimer = wl_event_loop_add_timer(loop, flashstep, NULL);
	draw.timer = wl_event_loop_add_timer(loop, boxtimeout, NULL);

	if (!(backend = wlr_backend_autocreate(loop, &session)))
		die("couldn't create backend");
	if (!(drw = fx_renderer_create(backend)) || !wlr_renderer_init_wl_shm(drw, dpy))
		die("couldn't create renderer");
	if (!(alloc = wlr_allocator_autocreate(backend, drw)))
		die("couldn't create allocator");

	scene = wlr_scene_create();
	bg = wlr_scene_rect_create(&scene->tree, 0, 0, (float[4]){ 0 });
	for (size_t i = 0; i < LENGTH(layers); i++)
		layers[i] = wlr_scene_tree_create(&scene->tree);
	draw.rect = wlr_scene_rect_create(layers[LyrTop], 0, 0, (float[4]){ 0 });
	draw.rect->accepts_input = false;
	wlr_scene_node_set_enabled(&draw.rect->node, false);
	alarmrect = wlr_scene_rect_create(layers[LyrOverlay], 0, 0, (float[4]){ 0.4, 0, 0, 0.4 });
	alarmrect->accepts_input = false;
	wlr_scene_node_set_enabled(&alarmrect->node, false);
	/* dmabuf feedback lets fullscreen apps skip compositing (direct scanout) */
	if (wlr_renderer_get_texture_formats(drw, WLR_BUFFER_CAP_DMABUF)) {
		wlr_drm_create(dpy, drw);
		wlr_scene_set_linux_dmabuf_v1(scene, wlr_linux_dmabuf_v1_create_with_renderer(dpy, 5, drw));
	}

	wlr_compositor_create(dpy, 6, drw);
	wlr_subcompositor_create(dpy);
	wlr_data_device_manager_create(dpy);
	wlr_primary_selection_v1_device_manager_create(dpy);
	wlr_data_control_manager_v1_create(dpy);
	wlr_ext_data_control_manager_v1_create(dpy, 1);
	wlr_viewporter_create(dpy);
	wlr_fractional_scale_manager_v1_create(dpy, 1);
	wlr_single_pixel_buffer_manager_v1_create(dpy);
	wlr_presentation_create(dpy, backend, 2);
	wlr_screencopy_manager_v1_create(dpy);
	wlr_ext_output_image_capture_source_manager_v1_create(dpy, 1);
	wlr_ext_image_copy_capture_manager_v1_create(dpy, 1);

	output_layout = wlr_output_layout_create(dpy);
	wl_signal_add(&output_layout->events.change, &layout_change);
	wl_signal_add(&backend->events.new_output, &new_output);
	wlr_xdg_output_manager_v1_create(dpy, output_layout);
	scene_layout = wlr_scene_attach_output_layout(scene, output_layout);
	wl_list_init(&mons);

	wl_list_init(&clients);
	xdg_shell = wlr_xdg_shell_create(dpy, 6);
	wl_signal_add(&xdg_shell->events.new_toplevel, &new_toplevel);
	wl_signal_add(&xdg_shell->events.new_popup, &new_popup);
	layer_shell = wlr_layer_shell_v1_create(dpy, 4);
	wl_signal_add(&layer_shell->events.new_surface, &new_layer);
	deco_mgr = wlr_xdg_decoration_manager_v1_create(dpy);
	wl_signal_add(&deco_mgr->events.new_toplevel_decoration, &new_deco);
	activation = wlr_xdg_activation_v1_create(dpy);
	wl_signal_add(&activation->events.request_activate, &request_activate);

	cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(cursor, output_layout);
	wl_signal_add(&cursor->events.motion, &cursor_motion);
	wl_signal_add(&cursor->events.motion_absolute, &cursor_motion_absolute);
	wl_signal_add(&cursor->events.button, &cursor_button);
	wl_signal_add(&cursor->events.axis, &cursor_axis);
	wl_signal_add(&cursor->events.frame, &cursor_frame);
	cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(dpy, 1);
	wl_signal_add(&cursor_shape_mgr->events.request_set_shape, &request_shape);

	wl_list_init(&keyboards);
	wl_list_init(&pointers);
	wl_signal_add(&backend->events.new_input, &new_input);
	seat = wlr_seat_create(dpy, "seat0");
	wl_signal_add(&seat->events.request_set_cursor, &request_cursor);
	wl_signal_add(&seat->events.request_set_selection, &request_sel);
	wl_signal_add(&seat->events.request_set_primary_selection, &request_psel);
	wlr_seat_set_capabilities(seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
}

static void
run(const char *startup)
{
	const char *socket = wl_display_add_socket_auto(dpy);

	if (!socket)
		die("couldn't create socket");
	setenv("WAYLAND_DISPLAY", socket, 1);
	config_init(wl_display_get_event_loop(dpy));
	if (!wlr_backend_start(backend))
		die("couldn't start backend");
	wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
	if (startup)
		spawn(startup);
	wlr_log(WLR_INFO, "running on WAYLAND_DISPLAY=%s", socket);
	wl_display_run(dpy);
}

static void
cleanup(void)
{
	/* wlroots asserts that no listeners are left when it destroys these */
	struct wl_listener *listeners[] = { &new_output, &new_input, &new_toplevel, &new_popup,
		&cursor_motion, &cursor_motion_absolute, &cursor_button, &cursor_axis, &cursor_frame,
		&request_cursor, &request_sel, &request_psel, &request_shape, &layout_change,
		&new_layer, &new_deco, &request_activate };

	wl_display_destroy_clients(dpy);
	config_finish();
	for (size_t i = 0; i < LENGTH(sigsrc); i++)
		wl_event_source_remove(sigsrc[i]);
	wl_event_source_remove(rep.timer);
	wl_event_source_remove(flashtimer);
	wl_event_source_remove(draw.timer);
	if (menu.fd >= 0) {
		wl_event_source_remove(menu.src);
		close(menu.fd);
	}
	if (errbox)
		wlr_scene_node_destroy(&errbox->node);
	fcft_destroy(font);
	fcft_fini();
	for (size_t i = 0; i < LENGTH(listeners); i++)
		wl_list_remove(&listeners[i]->link);
	wlr_scene_node_destroy(&scene->tree.node);
	wlr_xcursor_manager_destroy(cursor_mgr);
	wlr_cursor_destroy(cursor);
	wlr_allocator_destroy(alloc);
	wlr_renderer_destroy(drw);
	wlr_backend_destroy(backend);
	wl_display_destroy(dpy);
}

int
main(int argc, char *argv[])
{
	enum wlr_log_importance loglevel = WLR_INFO;
	const char *startup = NULL;
	int opt;

	while ((opt = getopt(argc, argv, "ds:v")) != -1) {
		if (opt == 'd') {
			loglevel = WLR_DEBUG;
		} else if (opt == 's') {
			startup = optarg;
		} else if (opt == 'v') {
			puts("dfwc-" VERSION);
			return 0;
		} else {
			goto usage;
		}
	}
	if (optind < argc)
		goto usage;
	wlr_log_init(loglevel, NULL);
	/* the outer desktop keeps Super for itself, so nested dfwc uses Alt */
	nested = getenv("WAYLAND_DISPLAY") || getenv("DISPLAY");
	setup();
	run(startup);
	cleanup();
	return 0;

usage:
	fprintf(stderr, "usage: dfwc [-dv] [-s startup command]\n");
	return 1;
}

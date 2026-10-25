/* See LICENSE file for copyright and license details.
 *
 * dfwc is a floating Wayland compositor on wlroots and scenefx. Windows float
 * and keep the stacking order the user gives them; keybinds and settings come
 * from init.lua (see lua.c).
 */
#include <drm_fourcc.h>
#include <errno.h>
#include <fcft/fcft.h>
#include <png.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdarg.h>
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
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/util/region.h>
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
#include <wlr/xwayland.h>

#include "dfwc.h"

#define MIN(A, B)               ((A) < (B) ? (A) : (B))
#define MAX(A, B)               ((A) > (B) ? (A) : (B))
#define LENGTH(X)               (sizeof(X) / sizeof((X)[0]))
#define LISTEN(E, L, H)         wl_signal_add((E), ((L)->notify = (H), (L)))
#define MODMASK                 (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)

enum { CurNormal, CurMove, CurResize, CurDraw, CurPan };
/* scene layers, bottom to top */
enum { LyrBg, LyrBottom, LyrClients, LyrOnTop, LyrTop, LyrFullscreen, LyrX11Menus, LyrOverlay, LyrLock, NumLayers };

typedef struct {
	uint32_t start, dur; /* ms; dur 0: not running */
} Anim;

typedef struct {
	bool tiling, canvas;
	char layout[64];
	double mfact;
	int nmaster;
} Mode; /* what each workspace keeps for itself; the shown one lives in settings */

typedef struct Workspace Workspace;

typedef struct {
	struct wlr_box out, hole;
	int ro[4], ri[4], angle;
	float col[2][4];
} GradKey; /* everything a gradient border's look depends on */

typedef struct {
	struct wl_list link; /* in clients, most recently focused first */
	struct wl_list tlink; /* in tiles: tiling order, newest first */
	struct wlr_xdg_toplevel *toplevel;      /* a Wayland app, or */
	struct wlr_xwayland_surface *xs;       /* an X11 app through XWayland */
	int unmanaged;       /* X11 menus and tooltips: placed by the app, never focused or framed */
	int associated;      /* the X11 window has its Wayland surface (and listeners on it) */
	struct wlr_scene_tree *scene;   /* at the window geometry's top-left: borders, then surf */
	struct wlr_scene_tree *surf;    /* the app's surfaces; wlroots offsets them by its geometry */
	struct wlr_scene_shadow *shadow; /* under floating windows */
	struct wlr_scene_blur *blur;     /* the blurred wallpaper behind the window */
	struct wlr_scene_rect *ring[2]; /* outer and inner border */
	struct wlr_scene_rect *arm[8];  /* corner brackets, for border_corners */
	struct wlr_scene_buffer *grad[4]; /* a gradient border instead of ring[0]: top, bottom, left, right */
	GradKey gradkey;     /* what grad[] was last drawn for */
	struct wlr_xdg_toplevel_decoration_v1 *deco;
	struct wlr_box prev; /* geometry to restore after maximize/fullscreen */
	struct wlr_box geom; /* geometry the borders were last drawn for */
	Style style;         /* last dfwc.style() result, applied on top of settings */
	float shown[3][4];   /* border colors on screen (from, inner, to): style's, or on their way there */
	float colfrom[3][4];
	float fade;          /* 0..1 while opening */
	int styled;          /* has been styled since it mapped */
	Anim open, col, mv;
	int fx, fy, tx, ty;  /* where a glide starts and ends */
	int maximized, fullscreen, ontop, minimized, radius;
	int floating;        /* stays floating while the rest tile */
	int tiled;           /* placed by tiling since it last floated; floatbox is valid */
	int smart;           /* the only tiled window on its screen, with smart_gaps */
	struct wlr_box floatbox; /* where it floated before tiling took it */
	uint32_t anchor;     /* edges a mouse resize grows, so the opposite ones stay put */
	int ax, ay;          /* right and bottom edges to keep while anchored */
	unsigned int id;     /* names the window in the restore menu */
	Workspace *ws;       /* while mapped */
	Rule rule;           /* what dfwc.rule() said when it opened */
	struct wl_listener map, unmap, commit, destroy, set_title, set_app_id;
	struct wl_listener request_move, request_resize, request_maximize, request_fullscreen;
	struct wl_listener deco_mode, deco_destroy;
	struct wl_listener associate, dissociate, configure, request_activate, set_geometry; /* X11 */
} Client;

typedef struct {
	struct wl_list link;
	struct wlr_scene_tree *tree;
	Anim a;
	float opacity;
	struct wlr_scene_rect *rect[10]; /* copies of the borders, with their colors */
	float color[10][4];
	size_t nrects;
	struct wlr_scene_shadow *shadow;
	float shadowcolor[4];
} Ghost; /* a closing window's last frame, fading out */

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
	struct wlr_scene_buffer *wall; /* built-in wallpaper */
	struct wlr_box wallbox;        /* area it was drawn for */
	struct wl_list spaces;         /* Workspace, top to bottom; the last one is empty */
	Workspace *ws;                 /* the one shown */
	Workspace *slidefrom;          /* sliding out while ws slides in */
	Anim slide;
	int slidedir;                  /* 1: the new one comes from below */
	struct wl_listener frame, request_state, destroy;
} Monitor;

struct Workspace {
	struct wl_list link;
	Monitor *m;
	struct wlr_scene_tree *tree[3]; /* its windows, under LyrClients, LyrOnTop, LyrFullscreen */
	Mode mode;
	int cx, cy; /* a canvas's view: the screen's top-left, in canvas coordinates */
};

typedef struct {
	struct wl_list link; /* in Monitor.layers */
	Monitor *mon;
	struct wlr_layer_surface_v1 *wlr;
	struct wlr_scene_layer_surface_v1 *scene;
	struct wl_listener map, unmap, commit, destroy;
} LayerSurface;

typedef struct {
	struct wlr_xdg_popup *wlr;
	struct wlr_scene_shadow *shadow;
	struct wl_listener commit, destroy;
} Popup;

typedef struct {
	struct wlr_pointer_constraint_v1 *wlr;
	struct wl_listener destroy;
} Constraint; /* a game locking or confining the pointer */

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
	void *claim;  /* the Client sized for it before it first showed */
} draw; /* drag-to-open */
static struct {
	struct wlr_scene_rect *rect;
	Anim a;
	struct wlr_box from, to;
} morph; /* the drawn box turning into the window that took it */
static struct {
	int fd;
	struct wl_event_source *src;
	char buf[64];
	size_t len;
} menu = { .fd = -1 }; /* restore menu being read */
static struct fcft_font *font;
static struct wlr_scene_tree *walltree; /* above the background color, below layer-shell wallpapers */
static struct wlr_scene_optimized_blur *optblur; /* the wallpaper, blurred once and cached */
static pixman_image_t *wallimg; /* decoded wallpaper PNG */
static struct wlr_output_layout *output_layout;
static struct wlr_scene_output_layout *scene_layout;
static struct wlr_xdg_shell *xdg_shell;
static struct wlr_layer_shell_v1 *layer_shell;
static struct wlr_xdg_decoration_manager_v1 *deco_mgr;
static struct wlr_xdg_activation_v1 *activation;
static struct wlr_compositor *compositor;
static struct wlr_relative_pointer_manager_v1 *relptr;
static struct wlr_pointer_constraints_v1 *constraints;
static struct wlr_pointer_constraint_v1 *activecon; /* the one under the pointer, if any */
static struct wlr_tearing_control_manager_v1 *tearingmgr;
static struct wlr_idle_notifier_v1 *idlenotifier;
static int inhibitors; /* idle inhibitors alive, e.g. a playing video */
static struct wlr_session_lock_v1 *lock; /* the running locker's lock */
static int locked; /* stays set when the locker dies, until a new one unlocks */
static struct wlr_scene_rect *lockbg; /* black over everything while locked */
static struct wlr_scene_tree *locktree; /* the locker's surfaces */
static struct wl_listener locksurf, lockunlock, lockdestroy;
static struct wlr_xwayland *xwayland;
static struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
static struct wlr_cursor *cursor;
static struct wlr_xcursor_manager *cursor_mgr;
static struct wlr_seat *seat;
static struct wl_list clients;
static struct wl_list tiles;
static struct wl_list mons;
static struct wl_list keyboards;
static struct wl_list pointers; /* libinput pointers, for touchpad settings */
static LayerSurface *exclusive; /* layer surface holding the keyboard, e.g. wmenu */
static int cycling; /* focus is walking the stack; the order commits when Mod is released */
static int repeating; /* the running bind comes from a held key */
static int animating; /* frames keep coming until every animation is done */
static struct wl_list ghosts;
static struct wl_list orphans; /* workspaces waiting for a screen after the last one went */
static Workspace *selws;       /* the workspace whose Mode settings holds */
static Workspace *panws;       /* the canvas being dragged */
static int tileddrag; /* the window being moved is tiled and stays tiled */
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
unlisten(struct wl_listener *l, ...)
{
	/* stop each listener; the list ends with NULL */
	va_list ap;

	va_start(ap, l);
	for (; l; l = va_arg(ap, struct wl_listener *))
		wl_list_remove(&l->link);
	va_end(ap);
}

/* animations: driven by output frames, so nothing runs while nothing moves */

static uint32_t
msec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static double
ease(double t)
{
	/* anim_curve as a CSS cubic-bezier: find u with x(u) = t, return y(u) */
	const float *p = settings.anim_curve;
	double u = t, x, dx;

	if (t <= 0 || t >= 1)
		return t <= 0 ? 0 : 1;
	for (int i = 0; i < 8; i++) { /* Newton; a few steps are plenty at screen precision */
		x = 3 * (1 - u) * (1 - u) * u * p[0] + 3 * (1 - u) * u * u * p[2] + u * u * u - t;
		dx = 3 * (1 - u) * (1 - u) * p[0] + 6 * (1 - u) * u * (p[2] - p[0]) + 3 * u * u * (1 - p[2]);
		if (fabs(x) < 1e-4 || fabs(dx) < 1e-6)
			break;
		u = MIN(MAX(u - x / dx, 0), 1);
	}
	return 3 * (1 - u) * (1 - u) * u * p[1] + 3 * (1 - u) * u * u * p[3] + u * u * u;
}

static void
wake(void)
{
	Monitor *m;

	if (animating++)
		return;
	wl_list_for_each(m, &mons, link) /* start the frame loop */
		wlr_output_schedule_frame(m->wlr);
}

static void
startanim(Anim *a, int dur)
{
	a->start = msec();
	a->dur = dur;
	wake();
}

static int
step(Anim *a, uint32_t now, double *e)
{
	/* eased progress in *e; returns whether it's still running */
	double p = a->dur ? (double)(now - a->start) / a->dur : 1;

	*e = ease(MIN(p, 1));
	if (p >= 1)
		a->dur = 0;
	return a->dur != 0;
}

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
	Client *c;

	if (!node)
		return NULL;
	if (node->type == WLR_SCENE_NODE_RECT || (node->type == WLR_SCENE_NODE_BUFFER
	&& !(ss = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node)))))
		return clientof(node); /* a border: the window, but no surface to send input to */
	if (node->type != WLR_SCENE_NODE_BUFFER)
		return NULL;
	*surface = ss->surface;
	c = clientof(node);
	return c && c->unmanaged ? NULL : c; /* X11 menus get the pointer, never focus */
}

static Client *clientfrom(struct wlr_surface *surface);
static void updateborders(Client *c);
static void restyle(Client *c);
static void placeerror(void);
static void drawwallpaper(Monitor *m);
static void snap(Client *c, int *x, int *y, int dx, int dy);
static void showmorph(double e);
static void focustop(void);
static void movetows(Client *c, Workspace *w);
static const struct wlr_buffer_impl textbufimpl;
static void constrain(struct wlr_pointer_constraint_v1 *con);
static void floatout(Client *c);
static int istiled(Client *c);

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

/* the few things that differ between Wayland apps and X11 apps */

static struct wlr_surface *
csurface(Client *c)
{
	return c->xs ? c->xs->surface : c->toplevel->base->surface;
}

static struct wlr_box
cgeom(Client *c)
{
	/* the visible window inside its surface; X11 windows have no shadow margin */
	return c->xs ? (struct wlr_box){ 0, 0, c->xs->width, c->xs->height } : c->toplevel->base->geometry;
}

static int
cmapped(Client *c)
{
	return csurface(c) && csurface(c)->mapped;
}

static struct wlr_box
oncanvas(Client *c, struct wlr_box b, int dir)
{
	/* screen to window coordinates (dir 1) or back (-1); they differ only on a
	 * panned canvas */
	if (c->ws) {
		b.x += dir * c->ws->cx;
		b.y += dir * c->ws->cy;
	}
	return b;
}

static void
cconfigure(Client *c, int x, int y, int w, int h)
{
	/* X11 windows get their screen position too, or their menus open in the wrong place */
	struct wlr_box b = oncanvas(c, (struct wlr_box){ x, y, w, h }, -1);

	if (c->xs)
		wlr_xwayland_surface_configure(c->xs, b.x, b.y, MAX(w, 1), MAX(h, 1));
	else
		wlr_xdg_toplevel_set_size(c->toplevel, MAX(w, 1), MAX(h, 1));
}

static void
cmove(Client *c, int x, int y)
{
	struct wlr_box g = cgeom(c), b = oncanvas(c, (struct wlr_box){ x, y, g.width, g.height }, -1);

	c->mv.dur = 0; /* a direct move ends any glide */
	wlr_scene_node_set_position(&c->scene->node, x, y);
	if (c->xs)
		wlr_xwayland_surface_configure(c->xs, b.x, b.y, g.width, g.height);
}

static void
cactivate(Client *c, int on)
{
	if (!c->xs) {
		wlr_xdg_toplevel_set_activated(c->toplevel, on);
		return;
	}
	wlr_xwayland_surface_activate(c->xs, on);
	if (on) /* X11 keeps its own stacking order */
		wlr_xwayland_surface_restack(c->xs, NULL, XCB_STACK_MODE_ABOVE);
}

static void
cmaximize(Client *c, int on)
{
	if (c->xs)
		wlr_xwayland_surface_set_maximized(c->xs, on, on);
	else
		wlr_xdg_toplevel_set_maximized(c->toplevel, on);
}

static void
cfullscreen(Client *c, int on)
{
	if (c->xs)
		wlr_xwayland_surface_set_fullscreen(c->xs, on);
	else
		wlr_xdg_toplevel_set_fullscreen(c->toplevel, on);
}

static void
cresizing(Client *c, int on)
{
	/* X11 has no such state; an unmapped Wayland window may take no configures */
	if (!c->xs && c->toplevel->base->initialized)
		wlr_xdg_toplevel_set_resizing(c->toplevel, on);
}

static void
cclose(Client *c)
{
	if (c->xs)
		wlr_xwayland_surface_close(c->xs);
	else
		wlr_xdg_toplevel_send_close(c->toplevel);
}

static const char *
cappid(Client *c)
{
	return c->xs ? c->xs->class : c->toplevel->app_id;
}

static const char *
ctitle(Client *c)
{
	return c->xs ? c->xs->title : c->toplevel->title;
}

static Client *
cparent(Client *c)
{
	if (c->xs)
		return c->xs->parent ? c->xs->parent->data : NULL;
	return c->toplevel->parent ? clientfrom(c->toplevel->parent->base->surface) : NULL;
}

static int
cfloats(Client *c)
{
	/* dialogs and fixed-size windows float even while tiling */
	xcb_size_hints_t *sh;

	if (!c->xs)
		return c->toplevel->parent || (c->toplevel->current.min_width
			&& c->toplevel->current.min_width == c->toplevel->current.max_width
			&& c->toplevel->current.min_height
			&& c->toplevel->current.min_height == c->toplevel->current.max_height);
	sh = c->xs->size_hints;
	return c->xs->parent || c->xs->modal
		|| wlr_xwayland_surface_has_window_type(c->xs, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG)
		|| wlr_xwayland_surface_has_window_type(c->xs, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY)
		|| wlr_xwayland_surface_has_window_type(c->xs, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR)
		|| wlr_xwayland_surface_has_window_type(c->xs, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH)
		|| (sh && sh->min_width > 0 && sh->min_width == sh->max_width
		&& sh->min_height > 0 && sh->min_height == sh->max_height);
}

static struct wlr_box
clientbox(Client *c)
{
	struct wlr_box g = cgeom(c);

	/* while gliding, where it's going is where it is */
	return c->mv.dur ? (struct wlr_box){ c->tx, c->ty, g.width, g.height }
		: (struct wlr_box){ c->scene->node.x, c->scene->node.y, g.width, g.height };
}

static void
resizeclient(Client *c, struct wlr_box b)
{
	/* shortcut: moves before the app has redrawn at the new size, so a window
	 * growing left/up shows a frame of misplaced content; fine for jumps
	 * like maximize, mouse resizes go through commitnotify instead */
	c->anchor = 0;
	if (settings.anim_move && cmapped(c) && (b.x != clientbox(c).x || b.y != clientbox(c).y)) {
		/* glide from wherever it is on screen now */
		c->fx = c->scene->node.x;
		c->fy = c->scene->node.y;
		c->tx = b.x;
		c->ty = b.y;
		startanim(&c->mv, settings.anim_move);
	} else if (!c->mv.dur) {
		wlr_scene_node_set_position(&c->scene->node, b.x, b.y);
	}
	cconfigure(c, b.x, b.y, b.width, b.height);
}

static void
extents(Client *c, int e[4])
{
	/* how far the borders reach out of the window: top, right, bottom, left */
	const Settings *s = &settings;

	for (int i = 0; i < 4; i++)
		e[i] = c->fullscreen || c->smart ? 0
			: s->border_width[i] + s->inner_border_width + s->border_gap;
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

/* workspaces */

static Workspace *
curws(void)
{
	/* the workspace shown on the screen with the pointer */
	Monitor *m = selmon();

	if (!m && !wl_list_empty(&mons))
		m = wl_container_of(mons.next, m, link);
	return m ? m->ws : NULL;
}

static int
shown(Client *c)
{
	return c->ws && c->ws->m && c->ws == c->ws->m->ws && !c->minimized;
}

static struct wlr_scene_tree *
clienttree(Client *c)
{
	/* a mapped window lives in its workspace's tree for its layer */
	int l = clientlayer(c);

	return c->ws ? c->ws->tree[l == LyrClients ? 0 : l == LyrOnTop ? 1 : 2] : layers[l];
}

static void
modeget(Mode *md)
{
	md->tiling = settings.tiling;
	md->canvas = settings.canvas;
	memcpy(md->layout, settings.layout, sizeof(md->layout));
	md->mfact = settings.mfact;
	md->nmaster = settings.nmaster;
}

static void
modeset(const Mode *md)
{
	settings.tiling = md->tiling;
	settings.canvas = md->canvas;
	memcpy(settings.layout, md->layout, sizeof(settings.layout));
	settings.mfact = md->mfact;
	settings.nmaster = md->nmaster;
}

static Workspace *
wsnew(Monitor *m)
{
	static const int lyr[] = { LyrClients, LyrOnTop, LyrFullscreen };
	const Settings *own = config_own();
	Workspace *w = calloc(1, sizeof(*w));

	if (!w)
		die("out of memory");
	w->m = m;
	for (int i = 0; i < 3; i++) {
		w->tree[i] = wlr_scene_tree_create(layers[lyr[i]]);
		wlr_scene_node_set_enabled(&w->tree[i]->node, false);
	}
	/* a new workspace starts as the config says */
	w->mode = (Mode){ .tiling = own->tiling, .canvas = own->canvas, .mfact = own->mfact,
		.nmaster = own->nmaster };
	memcpy(w->mode.layout, own->layout, sizeof(w->mode.layout));
	wl_list_insert(m->spaces.prev, &w->link);
	return w;
}

static void
wsplace(Workspace *w, int slide)
{
	for (int i = 0; i < 3; i++)
		wlr_scene_node_set_position(&w->tree[i]->node, -w->cx, -w->cy + slide);
}

static void
wsshow(Workspace *w, int on, int slide)
{
	for (int i = 0; i < 3; i++)
		wlr_scene_node_set_enabled(&w->tree[i]->node, on);
	wsplace(w, slide);
}

static void
pan(Workspace *w, int x, int y)
{
	/* one tree move however many windows; X11 apps are told where they now
	 * are, or their menus open in the wrong place */
	struct wlr_box b;
	Client *c;

	w->cx = x;
	w->cy = y;
	wsplace(w, 0);
	wl_list_for_each(c, &clients, link) {
		if (c->ws == w && c->xs) {
			b = oncanvas(c, clientbox(c), -1);
			wlr_xwayland_surface_configure(c->xs, b.x, b.y, b.width, b.height);
		}
	}
}

static void
unpan(Workspace *w)
{
	/* leaving canvas mode keeps windows where they are on screen, so floating
	 * and tiling never see an offset */
	int x = w->cx, y = w->cy;
	Client *c;

	w->cx = w->cy = 0;
	wsplace(w, 0);
	wl_list_for_each(c, &clients, link) {
		if (c->ws != w)
			continue;
		c->prev.x -= x;
		c->prev.y -= y;
		cmove(c, c->scene->node.x - x, c->scene->node.y - y);
	}
}

static int
wsempty(Workspace *w)
{
	Client *c;

	wl_list_for_each(c, &clients, link)
		if (c->ws == w)
			return 0;
	return 1;
}

static void
prune(Monitor *m)
{
	/* empty workspaces vanish once left, and one empty one always waits at the end */
	Workspace *w, *tmp;

	wl_list_for_each_safe(w, tmp, &m->spaces, link) {
		if (w == m->ws || w == m->slidefrom || w->link.next == &m->spaces || !wsempty(w))
			continue;
		for (int i = 0; i < 3; i++)
			wlr_scene_node_destroy(&w->tree[i]->node);
		wl_list_remove(&w->link);
		free(w);
	}
	if (!wsempty(wl_container_of(m->spaces.prev, w, link)))
		wsnew(m);
}

static Workspace *
wsat(Monitor *m, int n, int dir)
{
	/* the n-th workspace (the last if there are fewer), or the one dir steps away */
	struct wl_list *l;
	Workspace *w;

	if (!m)
		return NULL;
	if (dir) {
		l = dir > 0 ? m->ws->link.next : m->ws->link.prev;
		return l == &m->spaces ? NULL : wl_container_of(l, w, link);
	}
	for (l = m->spaces.next; --n > 0 && l->next != &m->spaces; l = l->next);
	return wl_container_of(l, w, link);
}

static int
wsindex(Workspace *w)
{
	Workspace *t;
	int i = 0;

	wl_list_for_each(t, &w->m->spaces, link) {
		if (t == w)
			break;
		i++;
	}
	return i;
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

	wlr_scene_buffer_set_opacity(buffer, (c->fullscreen ? 1 : c->style.opacity) * c->fade);
	/* round only the main surface; popups and subsurfaces keep their shape */
	if (ss && ss->surface == csurface(c))
		wlr_scene_buffer_set_corner_radius(buffer, c->radius);
}

static double
sdbox(double x, double y, struct wlr_box b, const int r[4])
{
	/* signed distance from x, y to a box with rounded corners (top-left,
	 * top-right, bottom-right, bottom-left): negative inside */
	double cx = b.x + b.width / 2.0, cy = b.y + b.height / 2.0;
	int rad = r[x < cx ? (y < cy ? 0 : 3) : (y < cy ? 1 : 2)];
	double qx = fabs(x - cx) - b.width / 2.0 + rad, qy = fabs(y - cy) - b.height / 2.0 + rad;

	return hypot(MAX(qx, 0), MAX(qy, 0)) + MIN(MAX(qx, qy), 0) - rad;
}

static void
gradstrip(struct wlr_scene_buffer *n, struct wlr_box s, const GradKey *k)
{
	/* one side of a gradient border, drawn on the CPU: the ring between k->out
	 * and k->hole, antialiased, colored along k->angle */
	double a = k->angle * M_PI / 180, dx = cos(a), dy = sin(a);
	double cx = k->out.x + k->out.width / 2.0, cy = k->out.y + k->out.height / 2.0;
	double half = fabs(k->out.width / 2.0 * dx) + fabs(k->out.height / 2.0 * dy);
	double x, y, cov, t;
	TextBuffer *b;
	uint32_t *px, v;
	int stride;
	float al;

	wlr_scene_node_set_enabled(&n->node, false);
	if (s.width <= 0 || s.height <= 0 || !(b = calloc(1, sizeof(*b))))
		return;
	if (!(b->img = pixman_image_create_bits(PIXMAN_a8r8g8b8, s.width, s.height, NULL, 0))) {
		free(b);
		return;
	}
	px = pixman_image_get_data(b->img);
	stride = pixman_image_get_stride(b->img) / 4;
	for (int j = 0; j < s.height; j++) {
		for (int i = 0; i < s.width; i++) {
			x = s.x + i + 0.5;
			y = s.y + j + 0.5;
			cov = MIN(MAX(0.5 - sdbox(x, y, k->out, k->ro), 0), 1)
				* MIN(MAX(0.5 + sdbox(x, y, k->hole, k->ri), 0), 1);
			t = half ? MIN(MAX(((x - cx) * dx + (y - cy) * dy) / (2 * half) + 0.5, 0), 1) : 0;
			al = (k->col[0][3] + (k->col[1][3] - k->col[0][3]) * t) * cov;
			v = (uint32_t)(al * 255 + 0.5f) << 24; /* premultiplied ARGB */
			for (int ch = 0; ch < 3; ch++)
				v |= (uint32_t)((k->col[0][ch] + (k->col[1][ch] - k->col[0][ch]) * t) * al * 255 + 0.5f)
					<< (16 - 8 * ch);
			px[j * stride + i] = v;
		}
	}
	wlr_buffer_init(&b->base, &textbufimpl, s.width, s.height);
	wlr_scene_buffer_set_buffer(n, &b->base);
	wlr_buffer_drop(&b->base);
	wlr_scene_node_set_position(&n->node, s.x, s.y);
	wlr_scene_node_set_enabled(&n->node, true);
}

static void
updateborders(Client *c)
{
	static const float red[4] = { 1, 0.2, 0.2, 1 };
	const Settings *s = &settings;
	struct wlr_box cg = cgeom(c), *geo = &cg, win = { 0, 0, geo->width, geo->height };
	float fo[4], fi[4], fe[4];
	const float *outer = fo, *inner = fi;
	GradKey k;
	int grad, ri, bot, right;
	const int *w = s->border_width, iw = s->inner_border_width, g = s->border_gap;
	struct wlr_box in, gap, out;
	float color[4];
	int bare = c->fullscreen || c->smart, R, L, sh;

	/* the colors on screen, faded in while the window opens */
	memcpy(fo, flashes % 2 ? red : c->shown[0], sizeof(fo));
	memcpy(fi, flashes % 2 ? red : c->shown[1], sizeof(fi));
	memcpy(fe, flashes % 2 ? red : c->shown[2], sizeof(fe));
	fo[3] *= c->fade;
	fi[3] *= c->fade;

	/* wlroots already offsets surf by the app's geometry, so the window's
	 * visible part starts at the root's origin */
	c->geom = *geo;
	/* square when maximized/fullscreen, and when the app draws its own shadows */
	c->radius = c->maximized || c->fullscreen || c->smart || geo->x || geo->y ? 0 : s->corner_radius;
	R = c->radius;

	/* from the window outwards: inner border, gap, outer border */
	in = (struct wlr_box){ -iw, -iw, win.width + 2 * iw, win.height + 2 * iw };
	gap = (struct wlr_box){ in.x - g, in.y - g, in.width + 2 * g, in.height + 2 * g };
	out = (struct wlr_box){ gap.x - w[3], gap.y - w[0],
		gap.width + w[1] + w[3], gap.height + w[0] + w[2] };

	/* a shadow under floating windows, cut out under the window itself so a
	 * see-through window never shows it */
	sh = s->shadow_size;
	wlr_scene_node_set_enabled(&c->shadow->node, sh && !bare && !c->tiled && !c->maximized);
	if (sh && !bare && !c->tiled && !c->maximized) {
		premul(s->shadow_color, color);
		for (int i = 0; i < 4; i++)
			color[i] *= c->fade;
		wlr_scene_node_set_position(&c->shadow->node,
			out.x - sh + s->shadow_offset[0], out.y - sh + s->shadow_offset[1]);
		wlr_scene_shadow_set_size(c->shadow, out.width + 2 * sh, out.height + 2 * sh);
		wlr_scene_shadow_set_blur_sigma(c->shadow, sh / 2.0f);
		wlr_scene_shadow_set_corner_radius(c->shadow, R ? R + iw + g + w[0] : 0);
		wlr_scene_shadow_set_color(c->shadow, color);
		wlr_scene_shadow_set_clipped_region(c->shadow, (struct clipped_region){
			{ sh - s->shadow_offset[0], sh - s->shadow_offset[1], out.width, out.height },
			corner_radii_all(R ? R + iw + g + w[0] : 0) });
	}
	/* blur only shows through see-through windows; opaque ones hide it for free */
	wlr_scene_node_set_enabled(&c->blur->node, s->blur && !c->fullscreen);
	wlr_scene_blur_set_size(c->blur, win.width, win.height);
	wlr_scene_blur_set_alpha(c->blur, c->fade); /* fades in with the window, not before it */
	wlr_scene_blur_set_corner_radius(c->blur, R);

	wlr_scene_node_set_enabled(&c->ring[1]->node, !bare && iw);
	if (!bare && iw)
		setring(c->ring[1], in, win, corner_radii_all(R ? R + iw : 0), R, inner);

	/* a two-color border is drawn into four strips; a solid one stays a rect */
	k = (GradKey){ .out = out, .hole = gap, .angle = s->border_angle };
	if (s->border_spin > 0) /* turned by the clock, so every window spins in step */
		k.angle = (k.angle + (int)(msec() % (uint32_t)(s->border_spin * 1000) * 360
			/ (s->border_spin * 1000))) % 360;
	for (int i = 0; R && i < 4; i++) {
		k.ri[i] = R + iw + g;
		k.ro[i] = k.ri[i] + MAX(w[i < 2 ? 0 : 2], w[i == 0 || i == 3 ? 3 : 1]);
	}
	/* unfaded: the strips fade by opacity, not by redrawing */
	memcpy(k.col[0], flashes % 2 ? red : c->shown[0], sizeof(k.col[0]));
	memcpy(k.col[1], fe, sizeof(fe));
	grad = !bare && !s->border_corners && memcmp(k.col[0], k.col[1], sizeof(k.col[0]));
	wlr_scene_node_set_enabled(&c->ring[0]->node, !bare && !s->border_corners && !grad);
	if (!bare && !s->border_corners && !grad)
		setring(c->ring[0], out, gap, R ? corner_radii_new(k.ro[0], k.ro[1], k.ro[2], k.ro[3])
			: corner_radii_none(), k.ri[0], outer);
	if (grad && memcmp(&k, &c->gradkey, sizeof(k))) {
		ri = k.ri[0];
		bot = gap.y + gap.height - ri;
		right = gap.x + gap.width;
		gradstrip(c->grad[0], (struct wlr_box){ out.x, out.y, out.width, gap.y + ri - out.y }, &k);
		gradstrip(c->grad[1], (struct wlr_box){ out.x, bot, out.width, out.y + out.height - bot }, &k);
		gradstrip(c->grad[2], (struct wlr_box){ out.x, gap.y + ri, gap.x - out.x, gap.height - 2 * ri }, &k);
		gradstrip(c->grad[3], (struct wlr_box){ right, gap.y + ri, out.x + out.width - right,
			gap.height - 2 * ri }, &k);
		c->gradkey = k;
	}
	for (int i = 0; i < 4; i++) {
		if (!grad)
			wlr_scene_node_set_enabled(&c->grad[i]->node, false);
		wlr_scene_buffer_set_opacity(c->grad[i], c->fade);
	}
	if (!grad)
		c->gradkey = (GradKey){ 0 };
	else if (s->border_spin > 0)
		wake(); /* animate() keeps turning it */

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
	int focused = f == csurface(c);
	WinInfo w = { cappid(c), ctitle(c), focused, c->maximized, c->fullscreen, c->ontop };

	if (c->unmanaged)
		return;

	memcpy(c->style.border, focused ? s->border_focused : s->border_unfocused, sizeof(c->style.border));
	memcpy(c->style.inner_border, focused ? s->inner_border_focused : s->inner_border_unfocused,
		sizeof(float[4]));
	c->style.opacity = s->opacity;
	config_style(&w, &c->style);
	if (s->anim_focus && c->styled && (memcmp(c->shown[0], c->style.border[0], sizeof(c->shown[0]))
	|| memcmp(c->shown[2], c->style.border[1], sizeof(c->shown[2])))) {
		memcpy(c->colfrom, c->shown, sizeof(c->colfrom)); /* blend from what's on screen */
		startanim(&c->col, s->anim_focus);
	} else if (!c->col.dur) {
		memcpy(c->shown[0], c->style.border[0], sizeof(c->shown[0]));
		memcpy(c->shown[1], c->style.inner_border, sizeof(c->shown[1]));
		memcpy(c->shown[2], c->style.border[1], sizeof(c->shown[2]));
	}
	c->styled = 1;
	updateborders(c);
}

static void
ghostnode(Ghost *g, struct wlr_scene_node *n, int x, int y)
{
	/* copy every buffer under n, hidden or not: unmapping already hid them */
	struct wlr_scene_buffer *b, *copy;
	struct wlr_scene_node *child;

	x += n->x;
	y += n->y;
	if (n->type == WLR_SCENE_NODE_TREE) {
		wl_list_for_each(child, &wlr_scene_tree_from_node(n)->children, link)
			ghostnode(g, child, x, y);
		return;
	}
	if (n->type != WLR_SCENE_NODE_BUFFER || !(b = wlr_scene_buffer_from_node(n))->buffer
	|| !(copy = wlr_scene_buffer_create(g->tree, b->buffer)))
		return;
	wlr_scene_node_set_position(&copy->node, x, y);
	wlr_scene_buffer_set_dest_size(copy, b->dst_width, b->dst_height);
	wlr_scene_buffer_set_source_box(copy, &b->src_box);
	wlr_scene_buffer_set_transform(copy, b->transform);
	wlr_scene_buffer_set_opacity(copy, g->opacity);
}

static void
ghost(Client *c)
{
	/* the window's last frame, kept to fade out after the app has let go */
	struct wlr_scene_rect *rects[LENGTH(c->ring) + LENGTH(c->arm)], *r;
	Ghost *g;

	if (!settings.anim_close || !c->scene->node.enabled || !shown(c) || !(g = calloc(1, sizeof(*g))))
		return;
	/* outside the workspace's tree, which may go before the fade ends */
	g->tree = wlr_scene_tree_create(layers[clientlayer(c)]);
	wlr_scene_node_set_position(&g->tree->node, c->scene->node.x, c->scene->node.y);
	if (c->shadow->node.enabled) {
		memcpy(g->shadowcolor, c->shadow->color, sizeof(g->shadowcolor));
		g->shadow = wlr_scene_shadow_create(g->tree, c->shadow->width, c->shadow->height,
			c->shadow->corner_radius, c->shadow->blur_sigma, g->shadowcolor);
		wlr_scene_node_set_position(&g->shadow->node, c->shadow->node.x, c->shadow->node.y);
		wlr_scene_shadow_set_clipped_region(g->shadow, c->shadow->clipped_region);
	}
	memcpy(rects, c->ring, sizeof(c->ring));
	memcpy(rects + LENGTH(c->ring), c->arm, sizeof(c->arm));
	for (size_t i = 0; i < LENGTH(rects); i++) {
		if (!rects[i]->node.enabled)
			continue;
		memcpy(g->color[g->nrects], rects[i]->color, sizeof(g->color[0]));
		r = g->rect[g->nrects++] = wlr_scene_rect_create(g->tree, rects[i]->width,
			rects[i]->height, rects[i]->color);
		wlr_scene_node_set_position(&r->node, rects[i]->node.x, rects[i]->node.y);
		wlr_scene_rect_set_corner_radii(r, rects[i]->corners);
		wlr_scene_rect_set_clipped_region(r, rects[i]->clipped_region);
	}
	g->opacity = (c->fullscreen ? 1 : c->style.opacity) * c->fade;
	for (size_t i = 0; i < LENGTH(c->grad); i++)
		if (c->grad[i]->node.enabled)
			ghostnode(g, &c->grad[i]->node, 0, 0);
	ghostnode(g, &c->surf->node, 0, 0);
	wl_list_insert(&ghosts, &g->link);
	startanim(&g->a, settings.anim_close);
}

static int
animate(void)
{
	/* one frame of every running animation; returns whether any still runs */
	uint32_t now = msec();
	struct wlr_scene_node *n;
	Ghost *g, *tmp;
	float color[4];
	int busy = 0, upd;
	double e;
	Client *c;
	Monitor *m;

	wl_list_for_each(c, &clients, link) {
		upd = 0;
		if (c->open.dur) {
			busy |= step(&c->open, now, &e);
			c->fade = e;
			upd = 1;
		}
		if (c->col.dur) {
			busy |= step(&c->col, now, &e);
			for (int i = 0; i < 4; i++) {
				c->shown[0][i] = c->colfrom[0][i] + (c->style.border[0][i] - c->colfrom[0][i]) * e;
				c->shown[1][i] = c->colfrom[1][i] + (c->style.inner_border[i] - c->colfrom[1][i]) * e;
				c->shown[2][i] = c->colfrom[2][i] + (c->style.border[1][i] - c->colfrom[2][i]) * e;
			}
			upd = 1;
		}
		if (settings.border_spin > 0 && c->gradkey.out.width)
			busy = upd = 1; /* a spinning gradient border */
		if (c->mv.dur) {
			busy |= step(&c->mv, now, &e);
			wlr_scene_node_set_position(&c->scene->node,
				c->fx + (c->tx - c->fx) * e, c->fy + (c->ty - c->fy) * e);
		}
		if (upd)
			updateborders(c);
	}
	if (morph.a.dur) {
		busy |= step(&morph.a, now, &e);
		showmorph(e);
	}
	wl_list_for_each(m, &mons, link) {
		if (!m->slide.dur)
			continue;
		busy |= step(&m->slide, now, &e);
		wsshow(m->ws, 1, m->slidedir * m->area.height * (1 - e));
		if (m->slide.dur) {
			wsshow(m->slidefrom, 1, -m->slidedir * m->area.height * e);
		} else {
			wsshow(m->slidefrom, 0, 0);
			m->slidefrom = NULL;
			prune(m);
		}
	}
	wl_list_for_each_safe(g, tmp, &ghosts, link) {
		if (!step(&g->a, now, &e)) {
			wlr_scene_node_destroy(&g->tree->node);
			wl_list_remove(&g->link);
			free(g);
			continue;
		}
		busy = 1;
		for (size_t i = 0; i < g->nrects; i++) {
			for (int j = 0; j < 4; j++)
				color[j] = g->color[i][j] * (1 - e);
			wlr_scene_rect_set_color(g->rect[i], color);
		}
		if (g->shadow) {
			for (int j = 0; j < 4; j++)
				color[j] = g->shadowcolor[j] * (1 - e);
			wlr_scene_shadow_set_color(g->shadow, color);
		}
		wl_list_for_each(n, &g->tree->children, link)
			if (n->type == WLR_SCENE_NODE_BUFFER)
				wlr_scene_buffer_set_opacity(wlr_scene_buffer_from_node(n), g->opacity * (1 - e));
	}
	return busy;
}

static Client *
clientfrom(struct wlr_surface *surface)
{
	/* the managed window a surface belongs to, if any */
	struct wlr_xwayland_surface *xs;
	struct wlr_xdg_toplevel *t;
	Client *c;

	if (!surface)
		return NULL;
	if ((xs = wlr_xwayland_surface_try_from_wlr_surface(surface)))
		return (c = xs->data) && !c->unmanaged ? c : NULL;
	t = wlr_xdg_toplevel_try_from_wlr_surface(surface);
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

static int
lockable(struct wlr_surface *s)
{
	/* while locked only the locker's surfaces may get input */
	return !locked || (s && wlr_session_lock_surface_v1_try_from_wlr_surface(s));
}

static void
kbfocus(struct wlr_surface *surface)
{
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	Client *old = selclient(), *c;

	if (!lockable(surface))
		return;
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
	Client *prev;

	if (!c)
		return;
	if (raise)
		wlr_scene_node_raise_to_top(&c->scene->node);
	if (exclusive || old == csurface(c))
		return;
	/* X11 windows that take no input (some tool palettes) never get the keyboard */
	if (c->xs && wlr_xwayland_surface_icccm_input_model(c->xs) == WLR_ICCCM_INPUT_MODEL_NONE)
		return;
	if ((prev = clientfrom(old)))
		cactivate(prev, 0);
	if (!cycling) {
		wl_list_remove(&c->link);
		wl_list_insert(&clients, &c->link);
	}
	cactivate(c, 1);
	kbfocus(csurface(c));
}

static void
focustop(void)
{
	Client *c;

	/* clear first so focusclient() doesn't deactivate a dying surface */
	wlr_seat_keyboard_notify_clear_focus(seat);
	/* shortcut: the cleared window isn't restyled; it's unmapping anyway */
	wl_list_for_each(c, &clients, link) {
		if (shown(c)) {
			focusclient(c, 0);
			return;
		}
	}
}

static void
floatout(Client *c)
{
	/* moving a tiled window by hand floats it where it is, like dwm */
	if (!settings.tiling || settings.canvas || c->floating)
		return;
	c->floating = 1;
	c->tiled = c->smart = 0;
	restyle(c);
	arrange();
}

static void
grab(Client *c, int mode, uint32_t edges)
{
	if (c->fullscreen)
		return;
	/* a tiled window being moved stays tiled, unless tiled_drag is "float" */
	tileddrag = mode == CurMove && istiled(c) && strcmp(settings.tiled_drag, "float");
	if (!tileddrag)
		floatout(c);
	if (c->maximized) { /* moving a maximized window keeps its size */
		c->maximized = 0;
		cmaximize(c, 0);
	}
	c->anchor = 0;
	c->mv.dur = 0;
	grabc = c;
	cursor_mode = mode;
	grabedges = edges;
	/* apps like foot hold their content still while this is set, instead of
	 * reflowing on every step */
	if (mode == CurResize)
		cresizing(c, 1);
	grabgeo = clientbox(c);
	grabx = cursor->x - (edges & WLR_EDGE_RIGHT ? grabgeo.x + grabgeo.width : grabgeo.x);
	graby = cursor->y - (edges & WLR_EDGE_BOTTOM ? grabgeo.y + grabgeo.height : grabgeo.y);
}

static void
swaptiles(Client *a, Client *b)
{
	/* exchange two windows' places in the tiling order */
	struct wl_list *pa = a->tlink.prev, *pb = b->tlink.prev;

	wl_list_remove(&a->tlink);
	wl_list_remove(&b->tlink);
	if (pa == &b->tlink) { /* b right before a */
		wl_list_insert(pb, &a->tlink);
		wl_list_insert(&a->tlink, &b->tlink);
	} else if (pb == &a->tlink) { /* a right before b */
		wl_list_insert(pa, &b->tlink);
		wl_list_insert(&b->tlink, &a->tlink);
	} else {
		wl_list_insert(pa, &b->tlink);
		wl_list_insert(pb, &a->tlink);
	}
}

static Client *
tiledat(double x, double y, Client *except)
{
	struct wlr_box b;
	Client *t;

	wl_list_for_each(t, &tiles, tlink) {
		b = outset(t, clientbox(t));
		if (t != except && shown(t) && istiled(t) && wlr_box_contains_point(&b, x, y))
			return t;
	}
	return NULL;
}

static void
ungrab(void)
{
	Client *c = grabc, *t = NULL;

	if (cursor_mode == CurResize)
		cresizing(grabc, 0);
	if (cursor_mode == CurPan)
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
	cursor_mode = CurNormal;
	grabc = NULL;
	if (!tileddrag)
		return;
	/* dropped on another tiled window: trade places; anywhere else: back to its tile */
	tileddrag = 0;
	if (!strcmp(settings.tiled_drag, "swap") && (t = tiledat(cursor->x, cursor->y, c)))
		swaptiles(c, t);
	arrange();
}

static Client *
neighbor(Client *c, int dx, int dy)
{
	/* the nearest tiled window in that direction, favoring ones in line */
	struct wlr_box a = clientbox(c), b;
	double ax = a.x + a.width / 2.0, ay = a.y + a.height / 2.0, along, across, best = INFINITY;
	Client *t, *found = NULL;

	wl_list_for_each(t, &tiles, tlink) {
		if (t == c || !istiled(t) || t->ws != c->ws)
			continue;
		b = clientbox(t);
		along = dx ? (b.x + b.width / 2.0 - ax) * (dx > 0 ? 1 : -1)
			: (b.y + b.height / 2.0 - ay) * (dy > 0 ? 1 : -1);
		across = dx ? fabs(b.y + b.height / 2.0 - ay) : fabs(b.x + b.width / 2.0 - ax);
		if (along > 0 && along + 2 * across < best) {
			best = along + 2 * across;
			found = t;
		}
	}
	return found;
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
		if (shown(n = wl_container_of(l, n, link)))
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
	if (istiled(c) && strcmp(settings.tiled_drag, "float")) {
		/* resizing a tiled window moves the master/stack split, so the window
		 * grows whichever side it's on; moving swaps it with its neighbor */
		Monitor *m = monof(c);
		Client *t;
		int n = 0, bstack = !strcmp(settings.layout, "bstack");
		double d;

		if ((dw || dh) && m) {
			wl_list_for_each(t, &tiles, tlink) {
				if (t == c)
					break;
				n += istiled(t) && t->ws == c->ws;
			}
			d = bstack ? (double)dh / m->usable.height : (double)dw / m->usable.width;
			settings.mfact = MIN(MAX(settings.mfact + (n < settings.nmaster ? d : -d), 0.05), 0.95);
			arrange();
		} else if (!dw && !dh && (t = repeating ? NULL : neighbor(c, dx, dy))) {
			swaptiles(c, t);
			arrange();
		}
		return;
	}
	if (c->maximized) {
		c->maximized = 0;
		cmaximize(c, 0);
	}
	floatout(c);
	b = clientbox(c);
	if (dw || dh) {
		resizeclient(c, (struct wlr_box){ b.x + dx, b.y + dy, b.width + dw, b.height + dh });
		return;
	}
	b.x += dx;
	b.y += dy;
	snap(c, &b.x, &b.y, dx, dy);
	cmove(c, b.x, b.y);
}

static void
setmaximized(Client *c, int on)
{
	Monitor *m = monof(c);

	if (on != c->maximized && !c->fullscreen && m) {
		if (on)
			c->prev = clientbox(c);
		c->maximized = on;
		resizeclient(c, on ? oncanvas(c, inset(c, m->usable), 1) : c->prev);
		restyle(c);
		arrange();
	}
	cmaximize(c, c->maximized);
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
		wlr_scene_node_reparent(&c->scene->node, clienttree(c));
		resizeclient(c, on ? oncanvas(c, m->area, 1)
			: c->maximized ? oncanvas(c, inset(c, m->usable), 1) : c->prev);
		restyle(c);
		arrange();
	}
	cfullscreen(c, c->fullscreen);
}

void
toggleontop(void)
{
	Client *c = selclient();

	if (!c)
		return;
	c->ontop = !c->ontop;
	wlr_scene_node_reparent(&c->scene->node, clienttree(c));
	restyle(c);
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
	arrange();
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
				movetows(c, curws());
				wlr_scene_node_set_enabled(&c->scene->node, true);
				focusclient(c, 1);
				arrange();
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
			ctitle(c) ? ctitle(c) : cappid(c) ? cappid(c) : "");
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
		cclose(c);
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
	if (sym == XKB_KEY_BackSpace && !locked) { /* killing dfwc must not open the lock */
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
	draw.claim = NULL;
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
		corner_radii_none(), 0, settings.border_focused[0]);
}

static void
showmorph(double e)
{
	/* the box's outline slides onto the window's frame and fades out */
	struct wlr_box b = {
		morph.from.x + (morph.to.x - morph.from.x) * e,
		morph.from.y + (morph.to.y - morph.from.y) * e,
		morph.from.width + (morph.to.width - morph.from.width) * e,
		morph.from.height + (morph.to.height - morph.from.height) * e,
	};
	float color[4];

	memcpy(color, settings.border_focused[0], sizeof(color));
	color[3] *= MAX(1 - e, 0);
	setring(morph.rect, b, (struct wlr_box){ b.x + 2, b.y + 2, b.width - 4, b.height - 4 },
		corner_radii_none(), 0, color);
	if (!morph.a.dur)
		wlr_scene_node_set_enabled(&morph.rect->node, false);
}

static int
tinybox(void)
{
	/* a click more than a drag: the app opens at its own size */
	return draw.box.width < 32 || draw.box.height < 32;
}

static int
snapok(int v, int step, int hold)
{
	/* mouse (no step): edges within settings.snap pull the window in.
	 * keys: the window stops at an edge it would cross; with hold it also
	 * stays there (v == -step: already flush), else the next move passes it */
	if (!step)
		return abs(v) <= settings.snap;
	return v * step < 0 && (hold ? abs(v) <= abs(step) : abs(v) < abs(step));
}

static void
snap(Client *c, int *x, int *y, int dx, int dy)
{
	/* line the window's outer edges up with screen edges and other windows' edges */
	struct wlr_box g = cgeom(c), *geo = &g;
	struct wlr_box me = outset(c, (struct wlr_box){ *x, *y, geo->width, geo->height }), o;
	Monitor *m = selmon();
	int bx = INT_MAX, by = INT_MAX;
	Client *t;

#define TRYX(v, hold) do { if (snapok(v, dx, hold) && abs(v) < abs(bx)) bx = (v); } while (0)
#define TRYY(v, hold) do { if (snapok(v, dy, hold) && abs(v) < abs(by)) by = (v); } while (0)
	if (!settings.snap)
		return;
	/* keys never push a window off the screen; a held key also stops at windows */
	if (m && !(c->ws && c->ws->mode.canvas)) { /* a canvas has no edges */
		o = m->usable;
		TRYX(o.x - me.x, 1);
		TRYX(o.x + o.width - me.x - me.width, 1);
		TRYY(o.y - me.y, 1);
		TRYY(o.y + o.height - me.y - me.height, 1);
	}
	wl_list_for_each(t, &clients, link) {
		if (t == c || !shown(t))
			continue;
		o = outset(t, clientbox(t));
		if (me.y < o.y + o.height && o.y < me.y + me.height) { /* side by side */
			TRYX(o.x + o.width - me.x, repeating);
			TRYX(o.x - me.x - me.width, repeating);
		}
		if (me.x < o.x + o.width && o.x < me.x + me.width) { /* stacked */
			TRYY(o.y + o.height - me.y, repeating);
			TRYY(o.y - me.y - me.height, repeating);
		}
	}
#undef TRYX
#undef TRYY
	if (bx != INT_MAX)
		*x += bx;
	if (by != INT_MAX)
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

	wlr_idle_notifier_v1_notify_activity(idlenotifier, seat);
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
			if (locked || !(handled = config_key(mods, syms[i])))
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

	int bound;

	repeating = 1;
	bound = !locked && kb && kb->repeat_info.rate > 0 && config_key(rep.mods, rep.sym);
	repeating = 0;
	if (bound)
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

	unlisten(&kb->modifiers, &kb->key, &kb->destroy, NULL);
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
		snap(grabc, &x, &y, 0, 0);
		cmove(grabc, x, y);
		return;
	}
	if (cursor_mode == CurPan) {
		pan(panws, grabx - cursor->x, graby - cursor->y);
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
		/* only ask for the size; commitnotify moves the window once the app
		 * has drawn it, keeping the opposite edges still */
		grabc->anchor = grabedges & (WLR_EDGE_LEFT | WLR_EDGE_TOP);
		grabc->ax = grabgeo.x + grabgeo.width;
		grabc->ay = grabgeo.y + grabgeo.height;
		cconfigure(grabc, l, t, r - l, b - t);
		return;
	}

	if ((c = clientat(cursor->x, cursor->y, &surface, &sx, &sy)))
		focusclient(c, 0); /* focus follows mouse, only a click raises */
	if (!surface) {
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
		wlr_seat_pointer_clear_focus(seat);
		constrain(NULL);
		return;
	}
	wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(seat, time, sx, sy);
	constrain(wlr_pointer_constraints_v1_constraint_for_surface(constraints, surface, seat));
}

static void
constrain(struct wlr_pointer_constraint_v1 *con)
{
	/* only the constraint of the surface under the pointer is active */
	if (activecon == con)
		return;
	if (activecon)
		wlr_pointer_constraint_v1_send_deactivated(activecon);
	if ((activecon = con))
		wlr_pointer_constraint_v1_send_activated(con);
}

static void
destroyconstraint(struct wl_listener *listener, void *data)
{
	Constraint *pc = wl_container_of(listener, pc, destroy);

	/* shortcut: the cursor stays where it is instead of warping to the app's hint */
	if (activecon == pc->wlr)
		activecon = NULL;
	wl_list_remove(&pc->destroy.link);
	free(pc);
}

static void
createconstraint(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_constraint_v1 *con = data;
	Constraint *pc = calloc(1, sizeof(*pc));

	if (!pc)
		return;
	pc->wlr = con;
	LISTEN(&con->events.destroy, &pc->destroy, destroyconstraint);
	if (seat->pointer_state.focused_surface == con->surface)
		constrain(con);
}

static void
motionrelative(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_motion_event *event = data;
	double dx = event->delta_x, dy = event->delta_y, sx, sy, cx, cy;

	wlr_idle_notifier_v1_notify_activity(idlenotifier, seat);

	/* games aim with the raw, unaccelerated movement */
	wlr_relative_pointer_manager_v1_send_relative_motion(relptr, seat,
		(uint64_t)event->time_msec * 1000, dx, dy, event->unaccel_dx, event->unaccel_dy);
	if (activecon && cursor_mode == CurNormal) {
		if (activecon->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
			return; /* the cursor stays put; the game only reads the movement */
		sx = seat->pointer_state.sx;
		sy = seat->pointer_state.sy;
		if (wlr_region_confine(&activecon->region, sx, sy, sx + dx, sy + dy, &cx, &cy)) {
			dx = cx - sx;
			dy = cy - sy;
		}
	}
	wlr_cursor_move(cursor, &event->pointer->base, dx, dy);
	motionnotify(event->time_msec);
}

static void
motionabsolute(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_motion_absolute_event *event = data;

	wlr_idle_notifier_v1_notify_activity(idlenotifier, seat);
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

	wlr_idle_notifier_v1_notify_activity(idlenotifier, seat);
	if (locked) { /* only the locker sees clicks; one gives its surface the keyboard */
		if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
			clientat(cursor->x, cursor->y, &surface, &sx, &sy);
			kbfocus(surface);
		}
		wlr_seat_pointer_notify_button(seat, event->time_msec, event->button, event->state);
		return;
	}
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
		&& event->button == BTN_RIGHT && (panws = curws()) && panws->mode.canvas) {
			/* Mod+right-drag on an empty canvas moves the view */
			cursor_mode = CurPan;
			grabx = cursor->x + panws->cx;
			graby = cursor->y + panws->cy;
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "grabbing");
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

	wlr_idle_notifier_v1_notify_activity(idlenotifier, seat);
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

static int
wantstearing(Monitor *m)
{
	/* the config allows it and a fullscreen app on this screen asks for it */
	Client *c;

	if (!settings.tearing)
		return 0;
	wl_list_for_each(c, &clients, link)
		if (c->fullscreen && cmapped(c) && shown(c) && c->ws->m == m)
			return wlr_tearing_control_manager_v1_surface_hint_from_surface(tearingmgr,
				csurface(c)) == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC;
	return 0;
}

static void
rendermon(struct wl_listener *listener, void *data)
{
	Monitor *m = wl_container_of(listener, m, frame);
	struct wlr_scene_output *so = wlr_scene_get_scene_output(scene, m->wlr);
	struct wlr_output_state state;
	struct timespec now;

	if (animating && (animating = animate()))
		wlr_output_schedule_frame(m->wlr); /* until the last animation ends */
	if (!wantstearing(m)) {
		wlr_scene_output_commit(so, NULL);
	} else if (wlr_scene_output_needs_frame(so)) {
		wlr_output_state_init(&state);
		if (wlr_scene_output_build_state(so, &state, NULL)) {
			state.tearing_page_flip = true;
			if (!wlr_output_test_state(m->wlr, &state))
				state.tearing_page_flip = false; /* the driver can't: vsync after all */
			wlr_output_commit_state(m->wlr, &state);
		}
		wlr_output_state_finish(&state);
	}
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
	Monitor *m = wl_container_of(listener, m, destroy), *o;
	LayerSurface *l, *tmp;
	Workspace *w, *wtmp;
	Client *c;

	wl_list_remove(&m->link);
	if (selws && selws->m == m)
		selws = NULL;
	if (!wl_list_empty(&mons)) {
		/* shortcut: windows keep their coordinates, so they may sit off the
		 * other screen; move them over by geometry once multi-monitor matters */
		o = wl_container_of(mons.next, o, link);
		wl_list_for_each(c, &clients, link)
			if (c->ws && c->ws->m == m)
				movetows(c, o->ws);
		wl_list_for_each_safe(w, wtmp, &m->spaces, link) {
			for (int i = 0; i < 3; i++)
				wlr_scene_node_destroy(&w->tree[i]->node);
			wl_list_remove(&w->link);
			free(w);
		}
	} else { /* the last screen: its workspaces wait for the next one */
		wl_list_for_each(w, &m->spaces, link)
			w->m = NULL;
		wl_list_insert_list(&orphans, &m->spaces);
	}
	if (m->wall)
		wlr_scene_node_destroy(&m->wall->node);
	for (size_t i = 0; i < LENGTH(m->layers); i++)
		wl_list_for_each_safe(l, tmp, &m->layers[i], link)
			wlr_layer_surface_v1_destroy(l->wlr);
	unlisten(&m->frame, &m->request_state, &m->destroy, NULL);
	free(m);
}

/* tiling */

static void
split(struct wlr_box a, int k, int gap, int vertical, struct wlr_box *out)
{
	/* k boxes side by side (or stacked), gap apart; the last takes the rounding */
	int len = vertical ? a.height : a.width, step = (len - (k - 1) * gap) / MAX(k, 1);

	for (int i = 0; i < k; i++) {
		out[i] = a;
		if (vertical) {
			out[i].y = a.y + i * (step + gap);
			out[i].height = i == k - 1 ? a.y + a.height - out[i].y : step;
		} else {
			out[i].x = a.x + i * (step + gap);
			out[i].width = i == k - 1 ? a.x + a.width - out[i].x : step;
		}
	}
}

static void
halve(struct wlr_box *a, double f, int gap, int vertical, int first, struct wlr_box *out)
{
	/* cut a into out (fraction f of it) and what's left (back into a); first
	 * puts out on the left/top side, else on the right/bottom */
	struct wlr_box parts[2];
	int len = (vertical ? a->height : a->width) - gap, n = len * f;

	parts[0] = parts[1] = *a;
	if (vertical) {
		parts[0].height = first ? n : len - n;
		parts[1].y = a->y + parts[0].height + gap;
		parts[1].height = a->height - parts[0].height - gap;
	} else {
		parts[0].width = first ? n : len - n;
		parts[1].x = a->x + parts[0].width + gap;
		parts[1].width = a->width - parts[0].width - gap;
	}
	*out = parts[!first];
	*a = parts[first];
}

static void
layout(const char *name, struct wlr_box a, int n, struct wlr_box *out)
{
	const Settings *s = &settings;
	int g = s->gaps_inner, nm = MIN(s->nmaster, n), cols, rows, i, k;
	struct wlr_box main = a, rest = a;

	if (!strcmp(name, "monocle")) {
		for (i = 0; i < n; i++)
			out[i] = a;
	} else if (!strcmp(name, "grid")) {
		for (cols = 1; cols * cols < n; cols++);
		rows = (n + cols - 1) / cols;
		struct wlr_box r[rows];
		split(a, rows, g, 1, r);
		for (i = 0; i < rows; i++) /* the last row may hold fewer, wider cells */
			split(r[i], MIN(cols, n - i * cols), g, 0, out + i * cols);
	} else if (!strcmp(name, "spiral") || !strcmp(name, "dwindle")) {
		/* each window takes a share of what's left: dwindle always cuts toward
		 * the bottom-right, spiral turns right, down, left, up */
		for (i = 0; i < n - 1; i++) {
			k = !strcmp(name, "spiral") ? i % 4 : i % 2;
			halve(&rest, i ? 0.5 : s->mfact, g, k % 2, k < 2, &out[i]);
		}
		out[n - 1] = rest;
	} else if (!strcmp(name, "centeredmaster") && n - nm > 1 && nm) {
		/* masters in the middle, the stack alternating right and left */
		int side = (a.width - a.width * s->mfact) / 2;
		struct wlr_box l = a, r = a, mid = a;
		l.width = r.width = MAX(side - g, 1);
		r.x = a.x + a.width - r.width;
		mid.x = l.x + l.width + g;
		mid.width = r.x - g - mid.x;
		split(mid, nm, g, 1, out);
		struct wlr_box lb[n], rb[n];
		k = (n - nm) / 2; /* left gets the smaller half */
		split(r, n - nm - k, g, 1, rb);
		split(l, k, g, 1, lb);
		for (i = 0; i < n - nm; i++)
			out[nm + i] = i % 2 ? lb[i / 2] : rb[i / 2];
	} else {
		/* tile (master left, stack right), bstack (master top), and the
		 * fallback for centeredmaster with one or no stack window */
		int v = !strcmp(name, "bstack");
		if (!nm || nm == n) {
			split(a, n, g, !v, out);
			return;
		}
		halve(&rest, s->mfact, g, v, 1, &main);
		split(main, nm, g, !v, out);
		split(rest, n - nm, g, !v, out + nm);
	}
}

static int
istiled(Client *c)
{
	return c->ws && c->ws->mode.tiling && !c->ws->mode.canvas && !c->floating && !c->minimized
		&& !c->fullscreen && !c->maximized;
}

static void
tile(Monitor *m)
{
	const Settings *s = &settings;
	struct wlr_box a, *out;
	Client *c;
	int n = 0, i = 0, smart;
	Mode keep;

	wl_list_for_each(c, &tiles, tlink)
		n += istiled(c) && c->ws == m->ws;
	if (!n || !(out = calloc(n, sizeof(*out))))
		return;
	modeget(&keep);
	modeset(&m->ws->mode); /* layout() and Lua layouts read settings */
	smart = s->smart_gaps && n == 1;
	a = m->usable;
	if (!smart)
		a = (struct wlr_box){ a.x + s->gaps_outer, a.y + s->gaps_outer,
			a.width - 2 * s->gaps_outer, a.height - 2 * s->gaps_outer };
	if (!config_layout(s->layout, a, n, out))
		layout(s->layout, a, n, out);
	wl_list_for_each(c, &tiles, tlink) {
		if (!istiled(c) || c->ws != m->ws)
			continue;
		if (!c->tiled || c->smart != smart) { /* borders, shadow and corners change */
			if (!c->tiled)
				c->floatbox = clientbox(c);
			c->tiled = 1;
			c->smart = smart;
			restyle(c);
		}
		resizeclient(c, inset(c, out[i++]));
	}
	free(out);
	modeset(&keep);
}

void
arrange(void)
{
	Workspace *w = curws();
	Monitor *m;
	Client *c;

	/* runtime changes (dfwc.tiling(), mfact keys, a reload) belong to the shown workspace */
	if (selws)
		modeget(&selws->mode);
	if (w && w != selws) {
		selws = w;
		modeset(&w->mode);
	}
	wl_list_for_each(m, &mons, link) {
		prune(m);
		wl_list_for_each(w, &m->spaces, link)
			if (!w->mode.canvas && (w->cx || w->cy))
				unpan(w);
	}
	/* tiling turned off: everything back where it floated */
	wl_list_for_each(c, &tiles, tlink) {
		if (c->tiled && !(c->ws && c->ws->mode.tiling && !c->ws->mode.canvas)) {
			c->tiled = c->smart = 0;
			resizeclient(c, c->floatbox);
			restyle(c);
		}
	}
	wl_list_for_each(m, &mons, link)
		tile(m);
}

static void
movetows(Client *c, Workspace *w)
{
	int dx, dy;

	if (!w || c->ws == w)
		return;
	dx = w->cx - (c->ws ? c->ws->cx : 0);
	dy = w->cy - (c->ws ? c->ws->cy : 0);
	c->ws = w;
	wlr_scene_node_reparent(&c->scene->node, clienttree(c));
	if (dx || dy) /* same place on screen */
		cmove(c, c->scene->node.x + dx, c->scene->node.y + dy);
}

static void
view(Workspace *w)
{
	Monitor *m = w ? w->m : NULL;
	Workspace *old;

	if (!m || w == m->ws)
		return;
	old = m->ws;
	if (m->slidefrom) /* a slide still running ends where it was going */
		wsshow(m->slidefrom, 0, 0);
	m->ws = w;
	m->slidefrom = NULL;
	if (settings.anim_move) {
		/* the old one slides out and the new one in, up or down by their order */
		m->slidefrom = old;
		m->slidedir = wsindex(w) > wsindex(old) ? 1 : -1;
		wsshow(w, 1, m->slidedir * m->area.height);
		startanim(&m->slide, settings.anim_move);
	} else {
		wsshow(old, 0, 0);
		wsshow(w, 1, 0);
	}
	focustop();
	arrange();
}

void
panby(int dx, int dy)
{
	Workspace *w = curws();

	if (w && w->mode.canvas)
		pan(w, w->cx + dx, w->cy + dy);
}

static void
swipe(struct wl_listener *listener, void *data)
{
	/* three fingers on the touchpad drag a canvas around */
	struct wlr_pointer_swipe_update_event *event = data;
	static double rx, ry; /* fractions of a pixel left from earlier events */

	wlr_idle_notifier_v1_notify_activity(idlenotifier, seat);
	if (event->fingers != 3 || locked)
		return;
	rx -= event->dx;
	ry -= event->dy;
	panby(rx, ry);
	rx -= (int)rx;
	ry -= (int)ry;
}

void
viewws(int n, int dir)
{
	Workspace *w = curws();

	view(wsat(w ? w->m : NULL, n, dir));
}

void
sendws(int n, int dir, int follow)
{
	Client *c = selclient();
	Workspace *w;

	if (!c || !c->ws || !(w = wsat(c->ws->m, n, dir)) || w == c->ws)
		return;
	movetows(c, w);
	if (follow)
		view(w); /* it was focused last, so it keeps the focus there */
	else
		focustop();
	arrange();
}

void
togglefloat(void)
{
	Client *c = selclient();

	if (!c)
		return;
	c->floating = !c->floating;
	if (c->floating && c->tiled) {
		c->tiled = c->smart = 0;
		resizeclient(c, c->floatbox);
		restyle(c);
	}
	arrange();
}

void
zoom(void)
{
	/* the focused window becomes the (first) master */
	Client *c = selclient();

	if (!c || !istiled(c))
		return;
	wl_list_remove(&c->tlink);
	wl_list_insert(&tiles, &c->tlink);
	arrange();
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
			resizeclient(c, oncanvas(c, c->fullscreen ? m->area : inset(c, m->usable), 1));
	tile(m);
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
	wlr_scene_node_set_position(&optblur->node, box.x, box.y);
	wlr_scene_optimized_blur_set_size(optblur, box.width, box.height);
	wl_list_for_each(m, &mons, link) {
		wlr_output_layout_get_box(output_layout, m->wlr, &m->area);
		arrangelayers(m);
		if (!wlr_box_equal(&m->area, &m->wallbox))
			drawwallpaper(m);
	}
	placeerror();
}

static void
createmon(struct wl_listener *listener, void *data)
{
	struct wlr_output *wlr = data;
	struct wlr_output_state state;
	struct wlr_output_mode *mode;
	Workspace *w;
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
	wl_list_init(&m->spaces);
	if (wl_list_empty(&orphans)) {
		m->ws = wsnew(m);
	} else { /* the last screen came back: its workspaces come with it */
		wl_list_insert_list(&m->spaces, &orphans);
		wl_list_init(&orphans);
		wl_list_for_each(w, &m->spaces, link) {
			w->m = m;
			wsshow(w, 0, 0);
		}
		m->ws = wl_container_of(m->spaces.next, w, link);
	}
	wsshow(m->ws, 1, 0);
	/* a new screen extends the desktop to the right; windows stay put */
	wlr_scene_output_layout_add_output(scene_layout,
		wlr_output_layout_add_auto(output_layout, wlr), wlr_scene_output_create(scene, wlr));
}

/* windows */

static void
mapnotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, map), *p;
	struct wlr_box g, *geo = &g, area, around, box;
	int w, h, claimed;

	static unsigned int ids;

	if (c->xs) { /* X11 surfaces come and go with each map */
		c->surf = wlr_scene_subsurface_tree_create(c->scene, c->xs->surface);
		c->surf->node.data = c;
		if (draw.pending && !draw.claim && !cparent(c))
			draw.claim = c; /* sized for the drawn box below, when it's placed */
		c->rule = (Rule){ 0 }; /* Wayland windows got theirs at the first commit */
		config_rule(&(WinInfo){ .app_id = cappid(c), .title = ctitle(c) }, &c->rule);
	}
	g = cgeom(c);
	c->ws = curws(); /* first: placement below is in its coordinates */
	c->id = ++ids;
	wl_list_insert(&clients, &c->link);
	wl_list_insert(&tiles, &c->tlink);
	/* dialogs, fixed-size windows, drawn boxes and rules float even while tiling */
	c->floating = cfloats(c) || (draw.claim == c && settings.tiling) || c->rule.floating;
	wlr_scene_node_set_enabled(&c->scene->node, true);
	outputbox(&area);
	area = oncanvas(c, inset(c, area), 1);
	if ((claimed = draw.claim == c)) {
		/* already sized for the drawn box; a click only sets the corner */
		morph.from = draw.box;
		around = oncanvas(c, draw.box, 1);
		box = inset(c, around);
		if (c->xs && !tinybox()) /* X11 apps aren't sized before they map */
			cconfigure(c, box.x, box.y, box.width, box.height);
		cmove(c, tinybox() ? around.x : box.x, tinybox() ? around.y : box.y);
		endbox();
	} else if (!wlr_box_empty(&area)) {
		/* where the rule says, else centered on the screen, dialogs on their
		 * parent; shrink to fit */
		around = area;
		if ((p = cparent(c)))
			around = clientbox(p);
		w = MIN(c->rule.width ? c->rule.width : geo->width, area.width);
		h = MIN(c->rule.height ? c->rule.height : geo->height, area.height);
		box.x = c->rule.pos ? area.x + c->rule.x : around.x + (around.width - w) / 2;
		box.y = c->rule.pos ? area.y + c->rule.y : around.y + (around.height - h) / 2;
		box.x = MIN(MAX(box.x, area.x), area.x + area.width - w);
		box.y = MIN(MAX(box.y, area.y), area.y + area.height - h);
		if (w != geo->width || h != geo->height)
			cconfigure(c, box.x, box.y, w, h);
		cmove(c, box.x, box.y);
	}
	c->ontop = c->rule.ontop;
	wlr_scene_node_reparent(&c->scene->node, clienttree(c));
	c->fade = settings.anim_open ? 0 : 1;
	if (settings.anim_open)
		startanim(&c->open, settings.anim_open);
	restyle(c);
	if (claimed && settings.anim_open) {
		morph.to = oncanvas(c, outset(c, clientbox(c)), -1);
		startanim(&morph.a, settings.anim_open);
		showmorph(0);
	}
	focusclient(c, 1);
	if (c->rule.fullscreen || (c->xs ? c->xs->fullscreen : c->toplevel->requested.fullscreen))
		setfullscreen(c, 1);
	else if (c->rule.maximized || (!claimed /* a drawn box beats the app's saved "maximized" */
	&& (c->xs ? c->xs->maximized_vert && c->xs->maximized_horz : c->toplevel->requested.maximized)))
		setmaximized(c, 1);
	arrange();
}

static void
unmapnotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, unmap);
	int focused = selclient() == c;

	ghost(c);
	c->open.dur = c->col.dur = c->mv.dur = 0;
	c->styled = 0;
	/* an app that hides and shows its window starts over as a plain one */
	c->maximized = c->fullscreen = c->minimized = c->tiled = c->smart = 0;
	wl_list_remove(&c->tlink);
	if (c == grabc)
		tileddrag = 0; /* its tile is gone: nothing to swap or snap back to */
	c->ws = NULL; /* out of its workspace, which may go */
	wlr_scene_node_reparent(&c->scene->node, clienttree(c));
	wlr_scene_node_set_enabled(&c->scene->node, false);
	if (c == grabc)
		ungrab();
	wl_list_remove(&c->link);
	if (focused)
		focustop();
	if (c->xs) {
		wlr_scene_node_destroy(&c->surf->node);
		c->surf = NULL;
	}
	arrange();
}

static void
commitnotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, commit);
	struct wlr_box g = cgeom(c), *geo = &g, box;

	if (cmapped(c)) {
		if (c->anchor) {
			cmove(c, c->anchor & WLR_EDGE_LEFT ? c->ax - geo->width : c->scene->node.x,
				c->anchor & WLR_EDGE_TOP ? c->ay - geo->height : c->scene->node.y);
			/* the drag is over and the app reached the last size asked for
			 * (X11 sizes change on request, not on commit) */
			if (c != grabc && (c->xs || (geo->width == c->toplevel->scheduled.width
			&& geo->height == c->toplevel->scheduled.height)))
				c->anchor = 0;
		}
		if (!wlr_box_equal(geo, &c->geom))
			updateborders(c);
		else if (c->radius || c->style.opacity < 1)
			wlr_scene_node_for_each_buffer(&c->surf->node, stylebuffer, c); /* new subsurfaces */
		return;
	}
	if (c->xs || !c->toplevel->base->initial_commit)
		return;
	c->rule = (Rule){ 0 };
	config_rule(&(WinInfo){ .app_id = cappid(c), .title = ctitle(c) }, &c->rule);
	if (draw.pending && !draw.claim && !c->toplevel->parent) {
		/* sized for the drawn box before it ever shows, so it doesn't jump */
		draw.claim = c;
		box = inset(c, draw.box);
		wlr_xdg_toplevel_set_size(c->toplevel, tinybox() ? 0 : MAX(box.width, 1),
			tinybox() ? 0 : MAX(box.height, 1));
	} else {
		/* the rule's size, or 0x0: the client picks its own */
		wlr_xdg_toplevel_set_size(c->toplevel, c->rule.width, c->rule.height);
	}
	/* "tiled" on every edge: GTK apps like Firefox drop their rounded corners
	 * and shadow margin, and take the size they're given over their saved one */
	if (wl_resource_get_version(c->toplevel->resource) >= XDG_TOPLEVEL_STATE_TILED_TOP_SINCE_VERSION)
		wlr_xdg_toplevel_set_tiled(c->toplevel,
			WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
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

	unlisten(&c->map, &c->unmap, &c->commit, &c->destroy, &c->request_move,
		&c->request_resize, &c->request_maximize, &c->request_fullscreen, &c->set_title,
		&c->set_app_id, NULL);
	wlr_scene_node_destroy(&c->scene->node);
	if (draw.claim == c)
		draw.claim = NULL; /* closed before showing: the next window gets the box */
	if (c->deco) /* its destroy fires after ours, when c is gone */
		unlisten(&c->deco_mode, &c->deco_destroy, NULL);
	free(c);
}

static void
retitle(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_title);

	if (cmapped(c) && config_hasstyle())
		restyle(c);
}

static void
reappid(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_app_id);

	if (cmapped(c) && config_hasstyle())
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

	unlisten(&c->deco_mode, &c->deco_destroy, NULL);
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
present(Client *c)
{
	/* an app asking for attention (a link opened in the browser) takes you to it */
	if (c->ws && c->ws->m && c->ws != c->ws->m->ws)
		view(c->ws);
	focusclient(c, 1);
}

static void
activate(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	Client *c = clientfrom(event->surface);

	if (c && cmapped(c))
		present(c);
}

static void
clientscene(Client *c, int layer, int framed)
{
	/* the window's root tree, hidden until it maps, with its border rects
	 * first so they sit below the surface added later */
	c->scene = wlr_scene_tree_create(layers[layer]);
	c->scene->node.data = c;
	c->fade = 1;
	wlr_scene_node_set_enabled(&c->scene->node, false);
	if (framed) {
		c->shadow = wlr_scene_shadow_create(c->scene, 0, 0, 0, 0, (float[4]){ 0 });
		c->blur = wlr_scene_blur_create(c->scene, 0, 0);
		/* sample the cached wallpaper blur instead of blurring live: nearly free */
		wlr_scene_blur_set_should_only_blur_bottom_layer(c->blur, true);
	}
	for (size_t i = 0; framed && i < LENGTH(c->ring); i++)
		c->ring[i] = wlr_scene_rect_create(c->scene, 0, 0, (float[4]){ 0 });
	for (size_t i = 0; framed && i < LENGTH(c->grad); i++)
		c->grad[i] = wlr_scene_buffer_create(c->scene, NULL);
	for (size_t i = 0; framed && i < LENGTH(c->arm); i++)
		c->arm[i] = wlr_scene_rect_create(c->scene, 0, 0, (float[4]){ 0 });
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
	clientscene(c, LyrClients, 1);
	c->surf = wlr_scene_xdg_surface_create(c->scene, toplevel->base);
	c->surf->node.data = c;
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
	struct wlr_box box, g = p->wlr->base->geometry;
	int sh = settings.shadow_size, *o = settings.shadow_offset, sx, sy;
	pixman_box32_t *e = pixman_region32_extents(&p->wlr->base->surface->opaque_region);
	struct wlr_box b = { e->x1 - g.x, e->y1 - g.y, e->x2 - e->x1, e->y2 - e->y1 };
	float color[4];

	if (p->wlr->base->initial_commit) {
		/* keep menus on screen; the box is relative to the toplevel's surface */
		if ((c = clientof(&tree->node))) {
			outputbox(&box);
			wlr_scene_node_coords(&c->scene->node, &sx, &sy); /* on screen, canvas or not */
			box.x -= sx - c->toplevel->base->geometry.x;
			box.y -= sy - c->toplevel->base->geometry.y;
			wlr_xdg_popup_unconstrain_from_box(p->wlr, &box);
		}
		wlr_xdg_surface_schedule_configure(p->wlr->base);
	}
	/* the window shadow under the menu's solid part (Firefox draws tooltips
	 * inside a see-through margin), cut out under it; skipped when the app
	 * leaves a margin for a shadow of its own, or says nothing is solid */
	wlr_scene_node_set_enabled(&p->shadow->node, sh && !g.x && !g.y && b.width > 0 && b.height > 0);
	if (!p->shadow->node.enabled)
		return;
	premul(settings.shadow_color, color);
	wlr_scene_node_set_position(&p->shadow->node, b.x + o[0] - sh, b.y + o[1] - sh);
	wlr_scene_shadow_set_size(p->shadow, b.width + 2 * sh, b.height + 2 * sh);
	wlr_scene_shadow_set_blur_sigma(p->shadow, sh / 2.0f);
	wlr_scene_shadow_set_color(p->shadow, color);
	wlr_scene_shadow_set_clipped_region(p->shadow, (struct clipped_region){
		{ sh - o[0], sh - o[1], b.width, b.height }, corner_radii_all(0) });
}

static void
destroypopup(struct wl_listener *listener, void *data)
{
	Popup *p = wl_container_of(listener, p, destroy);

	unlisten(&p->commit, &p->destroy, NULL);
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
	/* a child of the popup's tree, so wlroots places and frees it with the popup */
	p->shadow = wlr_scene_shadow_create(popup->base->data, 0, 0, 0, 0, (float[4]){ 0 });
	wlr_scene_node_lower_to_bottom(&p->shadow->node);
	LISTEN(&popup->base->surface->events.commit, &p->commit, commitpopup);
	LISTEN(&popup->events.destroy, &p->destroy, destroypopup);
}

/* X11 apps through XWayland */

static void
xconfigure(struct wl_listener *listener, void *data)
{
	/* an X11 app asks for a size or place: floating windows get it, the rest keep theirs */
	Client *c = wl_container_of(listener, c, configure);
	struct wlr_xwayland_surface_configure_event *e = data;
	struct wlr_box b;

	if (!cmapped(c) || !(c->tiled || c->maximized || c->fullscreen)) {
		wlr_xwayland_surface_configure(c->xs, e->x, e->y, e->width, e->height);
		if (cmapped(c))
			wlr_scene_node_set_position(&c->scene->node, e->x, e->y);
		return;
	}
	b = clientbox(c);
	wlr_xwayland_surface_configure(c->xs, b.x, b.y, b.width, b.height);
}

static void
xactivate(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, request_activate);

	if (cmapped(c) && !c->unmanaged)
		present(c);
}

static void
xresize(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, request_resize);
	struct wlr_xwayland_resize_event *event = data;

	if (cmapped(c))
		grab(c, CurResize, event->edges);
}

static void
xmaximize(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, request_maximize);

	if (cmapped(c))
		setmaximized(c, c->xs->maximized_vert && c->xs->maximized_horz);
}

static void
xfullscreen(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, request_fullscreen);

	if (cmapped(c))
		setfullscreen(c, c->xs->fullscreen);
}

static void
xgeometry(struct wl_listener *listener, void *data)
{
	/* menus and tooltips place themselves */
	Client *c = wl_container_of(listener, c, set_geometry);

	if (c->unmanaged && cmapped(c))
		wlr_scene_node_set_position(&c->scene->node, c->xs->x, c->xs->y);
}

static void
mapunmanaged(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, map);

	c->surf = wlr_scene_subsurface_tree_create(c->scene, c->xs->surface);
	c->surf->node.data = c;
	wlr_scene_node_set_position(&c->scene->node, c->xs->x, c->xs->y);
	wlr_scene_node_set_enabled(&c->scene->node, true);
	if (wlr_xwayland_surface_override_redirect_wants_focus(c->xs))
		kbfocus(c->xs->surface);
}

static void
unmapunmanaged(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, unmap);

	wlr_scene_node_set_enabled(&c->scene->node, false);
	wlr_scene_node_destroy(&c->surf->node);
	c->surf = NULL;
	if (seat->keyboard_state.focused_surface == c->xs->surface)
		focustop();
}

static void
xassociate(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, associate);

	c->associated = 1;
	LISTEN(&c->xs->surface->events.map, &c->map, c->unmanaged ? mapunmanaged : mapnotify);
	LISTEN(&c->xs->surface->events.unmap, &c->unmap, c->unmanaged ? unmapunmanaged : unmapnotify);
	LISTEN(&c->xs->surface->events.commit, &c->commit, commitnotify);
}

static void
xdissociate(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, dissociate);

	c->associated = 0;
	unlisten(&c->map, &c->unmap, &c->commit, NULL);
}

static void
destroyx11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, destroy);

	if (c->associated)
		xdissociate(&c->dissociate, NULL);
	unlisten(&c->associate, &c->dissociate, &c->destroy, &c->configure, &c->request_activate,
		&c->request_move, &c->request_resize, &c->request_maximize, &c->request_fullscreen,
		&c->set_title, &c->set_app_id, &c->set_geometry, NULL);
	wlr_scene_node_destroy(&c->scene->node);
	if (draw.claim == c)
		draw.claim = NULL;
	c->xs->data = NULL;
	free(c);
}

static void
createx11(struct wl_listener *listener, void *data)
{
	struct wlr_xwayland_surface *xs = data;
	Client *c = calloc(1, sizeof(*c));

	if (!c) {
		wlr_log(WLR_ERROR, "out of memory for an X11 window");
		return;
	}
	c->xs = xs;
	xs->data = c;
	c->unmanaged = xs->override_redirect;
	clientscene(c, c->unmanaged ? LyrX11Menus : LyrClients, !c->unmanaged);

	LISTEN(&xs->events.associate, &c->associate, xassociate);
	LISTEN(&xs->events.dissociate, &c->dissociate, xdissociate);
	LISTEN(&xs->events.destroy, &c->destroy, destroyx11);
	LISTEN(&xs->events.request_configure, &c->configure, xconfigure);
	LISTEN(&xs->events.request_activate, &c->request_activate, xactivate);
	LISTEN(&xs->events.request_move, &c->request_move, requestmove);
	LISTEN(&xs->events.request_resize, &c->request_resize, xresize);
	LISTEN(&xs->events.request_maximize, &c->request_maximize, xmaximize);
	LISTEN(&xs->events.request_fullscreen, &c->request_fullscreen, xfullscreen);
	LISTEN(&xs->events.set_title, &c->set_title, retitle);
	LISTEN(&xs->events.set_class, &c->set_app_id, reappid);
	LISTEN(&xs->events.set_geometry, &c->set_geometry, xgeometry);
}

static void
xready(struct wl_listener *listener, void *data)
{
	struct wlr_xcursor *xc;

	wlr_xwayland_set_seat(xwayland, seat);
	/* the cursor X11 apps show where they don't set their own */
	if ((xc = wlr_xcursor_manager_get_xcursor(cursor_mgr, "default", 1)))
		wlr_xwayland_set_cursor(xwayland, wlr_xcursor_image_get_buffer(xc->images[0]),
			xc->images[0]->hotspot_x, xc->images[0]->hotspot_y);
}

/* layer surfaces: wallpapers, bars, launchers */

static void
maplayer(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, map);

	if (l->wlr->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
		exclusive = l;
		kbfocus(l->wlr->surface);
		/* the drawn box waits as long as the launcher is open */
		if (draw.pending)
			wl_event_source_timer_update(draw.timer, 0);
	}
	arrangelayers(l->mon);
}

static void
unmaplayer(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, unmap);

	if (exclusive == l) {
		exclusive = NULL;
		/* launcher closed: picked an app or escaped, dfwc can't tell. Hide the
		 * box, but a window opening within box_timeout still lands in it */
		if (draw.pending && !draw.claim) {
			wlr_scene_node_set_enabled(&draw.rect->node, false);
			wl_event_source_timer_update(draw.timer, settings.box_timeout);
		}
	}
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

	/* a new wallpaper frame (swaybg and friends): the blur cache is stale */
	if (ls->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND)
		wlr_scene_optimized_blur_mark_dirty(optblur);
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
	unlisten(&l->map, &l->unmap, &l->commit, &l->destroy, NULL);
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
	if (strcmp(s->wallpaper, prev.wallpaper))
		wl_list_for_each(m, &mons, link)
			drawwallpaper(m);
	wlr_scene_set_blur_data(scene, s->blur_passes, s->blur_radius, 0.02, 0.9, 0.9, 1.1);
	wlr_scene_node_set_enabled(&optblur->node, s->blur);
	if (memcmp(s->background, prev.background, sizeof(s->background)))
		wlr_scene_optimized_blur_mark_dirty(optblur);

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
	arrange(); /* gaps, layout or tiling itself may have changed */
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

static int
drawtext(pixman_image_t *dst, struct fcft_font *f, const char *str, int x, int y,
	pixman_color_t *color, int maxw)
{
	/* UTF-8 text with its top-left at x, y, stopping before maxw (0: no limit);
	 * dst NULL only measures. Returns the width it covers */
	pixman_image_t *fg = dst ? pixman_image_create_solid_fill(color) : NULL;
	const struct fcft_glyph *g;
	mbstate_t ps = { 0 };
	char32_t cp;
	size_t len;
	int w = 0;

	for (const char *p = str; *p; p += len) {
		if ((len = mbrtoc32(&cp, p, strlen(p), &ps)) == 0 || len > 4) { /* invalid UTF-8 */
			cp = '?';
			len = 1;
			ps = (mbstate_t){ 0 };
		}
		if (!(g = fcft_rasterize_char_utf32(f, cp, FCFT_SUBPIXEL_NONE)))
			continue;
		if (maxw && w + g->advance.x > maxw)
			break;
		if (fg) /* color glyphs (emoji) are images; the rest are masks for the color */
			pixman_image_composite32(PIXMAN_OP_OVER, g->is_color_glyph ? g->pix : fg,
				g->is_color_glyph ? NULL : g->pix, dst, 0, 0, 0, 0,
				x + w + g->x, y + f->ascent - g->y, g->width, g->height);
		w += g->advance.x;
	}
	if (fg)
		pixman_image_unref(fg);
	return w;
}

static pixman_image_t *
wallimage(void)
{
	/* the wallpaper's PNG, decoded once and kept until the setting changes */
	static char loaded[sizeof(settings.wallpaper) + 64];
	char path[sizeof(loaded)];
	png_image png = { .version = PNG_IMAGE_VERSION };
	uint32_t *px;
	int alpha;

	if (strchr(settings.wallpaper, '/'))
		snprintf(path, sizeof(path), "%s", settings.wallpaper);
	else
		snprintf(path, sizeof(path), PREFIX "/share/dfwc/wallpapers/%s.png", settings.wallpaper);
	if (wallimg && !strcmp(path, loaded))
		return wallimg;
	if (wallimg)
		pixman_image_unref(wallimg);
	wallimg = NULL;
	snprintf(loaded, sizeof(loaded), "%s", path);
	if (!png_image_begin_read_from_file(&png, path)) {
		wlr_log(WLR_ERROR, "wallpaper %s: %s", path, png.message);
		return NULL;
	}
	alpha = png.format & PNG_FORMAT_FLAG_ALPHA;
	png.format = PNG_FORMAT_BGRA; /* byte order of little-endian a8r8g8b8 */
	if (!(wallimg = pixman_image_create_bits(PIXMAN_a8r8g8b8, png.width, png.height, NULL, 0))
	|| !png_image_finish_read(&png, NULL, pixman_image_get_data(wallimg), pixman_image_get_stride(wallimg), NULL)) {
		wlr_log(WLR_ERROR, "wallpaper %s: %s", path, png.message);
		png_image_free(&png);
		if (wallimg)
			pixman_image_unref(wallimg);
		return wallimg = NULL;
	}
	if (alpha) /* pixman wants premultiplied color */
		for (px = pixman_image_get_data(wallimg); px < pixman_image_get_data(wallimg) + png.width * png.height; px++) {
			uint32_t a = *px >> 24;
			*px = a << 24 | ((*px >> 16 & 0xff) * a / 255) << 16 | ((*px >> 8 & 0xff) * a / 255) << 8
				| (*px & 0xff) * a / 255;
		}
	return wallimg;
}

static void
drawwallpaper(Monitor *m)
{
	/* the PNG scaled to cover the screen, cropped evenly, drawn once per
	 * screen size; without one the background color shows, swaybg draws over it */
	int w = m->wlr->width, h = m->wlr->height;
	pixman_image_t *src;
	pixman_transform_t tr;
	TextBuffer *t;
	double sc;

	if (m->wall) {
		wlr_scene_node_destroy(&m->wall->node);
		m->wall = NULL;
	}
	m->wallbox = m->area;
	if (!*settings.wallpaper || w <= 0 || h <= 0 || !(src = wallimage()) || !(t = calloc(1, sizeof(*t))))
		return;
	if (!(t->img = pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, NULL, 0))) {
		free(t);
		return;
	}
	sc = MAX((double)w / pixman_image_get_width(src), (double)h / pixman_image_get_height(src));
	pixman_transform_init_scale(&tr, pixman_double_to_fixed(1 / sc), pixman_double_to_fixed(1 / sc));
	pixman_image_set_transform(src, &tr);
	pixman_image_set_filter(src, PIXMAN_FILTER_GOOD, NULL, 0);
	pixman_image_composite32(PIXMAN_OP_SRC, src, NULL, t->img,
		(pixman_image_get_width(src) * sc - w) / 2, (pixman_image_get_height(src) * sc - h) / 2,
		0, 0, 0, 0, w, h);
	wlr_buffer_init(&t->base, &textbufimpl, w, h);
	if ((m->wall = wlr_scene_buffer_create(walltree, &t->base))) {
		wlr_scene_buffer_set_dest_size(m->wall, m->area.width, m->area.height);
		wlr_scene_node_set_position(&m->wall->node, m->area.x, m->area.y);
	}
	wlr_buffer_drop(&t->base);
	wlr_scene_optimized_blur_mark_dirty(optblur);
}

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
	TextBuffer *t;
	struct wlr_box area;
	int w, h, maxw;

	if (errbox) {
		wlr_scene_node_destroy(&errbox->node);
		errbox = NULL;
	}
	/* shortcut: drawn at scale 1; pass the output's dpi to fcft for crisp HiDPI text */
	if (!msg || (!font && !(font = fcft_from_name(1, (const char *[]){ "monospace:size=11" }, NULL))))
		return;
	outputbox(&area);
	maxw = area.width ? area.width - 2 * Pad : 0;
	w = drawtext(NULL, font, msg, 0, 0, NULL, maxw) + 2 * Pad;
	h = font->height + 2 * Pad;
	if (!(t = calloc(1, sizeof(*t)))
	|| !(t->img = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, w, h, NULL, 0))) {
		free(t);
		return;
	}
	/* black at 85%, so the windows behind still show a little */
	pixman_image_fill_rectangles(PIXMAN_OP_SRC, t->img, &(pixman_color_t){ 0, 0, 0, 0xd999 },
		1, &(pixman_rectangle16_t){ 0, 0, w, h });
	drawtext(t->img, font, msg, Pad, Pad, &white, maxw);
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
static struct wl_listener cursor_swipe = { .notify = swipe };
static struct wl_listener request_cursor = { .notify = setcursor };
static struct wl_listener request_sel = { .notify = setsel };
static struct wl_listener request_psel = { .notify = setpsel };
static struct wl_listener request_shape = { .notify = setcursorshape };
static struct wl_listener layout_change = { .notify = updatemons };
static struct wl_listener new_layer = { .notify = createlayer };
static struct wl_listener new_deco = { .notify = createdeco };
static struct wl_listener request_activate = { .notify = activate };
static struct wl_listener new_xsurface = { .notify = createx11 };
static struct wl_listener new_constraint = { .notify = createconstraint };
static struct wl_listener xwayland_ready = { .notify = xready };

static int
quitsignal(int sig, void *data)
{
	quit();
	return 0;
}

/* lock screen (swaylock) and idle (swayidle) */

static void
newlocksurface(struct wl_listener *listener, void *data)
{
	struct wlr_session_lock_surface_v1 *ls = data;
	struct wlr_scene_tree *t = wlr_scene_subsurface_tree_create(locktree, ls->surface);
	struct wlr_surface *f = seat->keyboard_state.focused_surface;
	struct wlr_box b;

	wlr_output_layout_get_box(output_layout, ls->output, &b);
	wlr_scene_node_set_position(&t->node, b.x, b.y);
	wlr_session_lock_surface_v1_configure(ls, b.width, b.height);
	/* shortcut: if the focused one goes (screen unplugged), typing needs a click on another */
	if (!f || !wlr_session_lock_surface_v1_try_from_wlr_surface(f))
		kbfocus(ls->surface);
}

static void
endlock(int unlocked)
{
	unlisten(&locksurf, &lockunlock, &lockdestroy, NULL);
	wlr_scene_node_destroy(&locktree->node);
	lock = NULL;
	wlr_seat_keyboard_notify_clear_focus(seat);
	if (!unlocked)
		return; /* the locker died: stay black and locked until a new one unlocks */
	locked = 0;
	wlr_scene_node_set_enabled(&lockbg->node, false);
	if (exclusive)
		kbfocus(exclusive->wlr->surface);
	else
		focustop();
	motionnotify(0); /* the pointer goes back to what's under it */
}

static void
unlocksession(struct wl_listener *listener, void *data)
{
	endlock(1);
}

static void
destroylock(struct wl_listener *listener, void *data)
{
	endlock(0);
}

static void
newlock(struct wl_listener *listener, void *data)
{
	struct wlr_session_lock_v1 *l = data;

	if (lock) { /* one locker at a time */
		wlr_session_lock_v1_destroy(l);
		return;
	}
	lock = l;
	locked = 1;
	endbox();
	ungrab();
	constrain(NULL);
	wlr_seat_keyboard_notify_clear_focus(seat);
	wlr_seat_pointer_clear_focus(seat);
	wlr_scene_node_set_enabled(&lockbg->node, true);
	locktree = wlr_scene_tree_create(layers[LyrLock]);
	LISTEN(&l->events.new_surface, &locksurf, newlocksurface);
	LISTEN(&l->events.unlock, &lockunlock, unlocksession);
	LISTEN(&l->events.destroy, &lockdestroy, destroylock);
	wlr_session_lock_v1_send_locked(l);
}

static void
uninhibit(struct wl_listener *listener, void *data)
{
	wl_list_remove(&listener->link);
	free(listener);
	wlr_idle_notifier_v1_set_inhibited(idlenotifier, --inhibitors > 0);
}

static void
inhibit(struct wl_listener *listener, void *data)
{
	struct wlr_idle_inhibitor_v1 *i = data;
	struct wl_listener *l = calloc(1, sizeof(*l));

	if (!l)
		return;
	/* shortcut: hidden and minimized windows inhibit too; check visibility if
	 * a video playing in the background keeps the screen on */
	LISTEN(&i->events.destroy, l, uninhibit);
	wlr_idle_notifier_v1_set_inhibited(idlenotifier, ++inhibitors > 0);
}

static struct wl_listener new_lock = { .notify = newlock };
static struct wl_listener new_inhibitor = { .notify = inhibit };

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
	walltree = wlr_scene_tree_create(&scene->tree);
	for (size_t i = 0; i < LENGTH(layers); i++)
		layers[i] = wlr_scene_tree_create(&scene->tree);
	draw.rect = wlr_scene_rect_create(layers[LyrTop], 0, 0, (float[4]){ 0 });
	draw.rect->accepts_input = false;
	wlr_scene_node_set_enabled(&draw.rect->node, false);
	morph.rect = wlr_scene_rect_create(layers[LyrTop], 0, 0, (float[4]){ 0 });
	morph.rect->accepts_input = false;
	wlr_scene_node_set_enabled(&morph.rect->node, false);
	optblur = wlr_scene_optimized_blur_create(layers[LyrBottom], 0, 0);
	wlr_scene_node_lower_to_bottom(&optblur->node); /* right above every wallpaper */
	alarmrect = wlr_scene_rect_create(layers[LyrOverlay], 0, 0, (float[4]){ 0.4, 0, 0, 0.4 });
	alarmrect->accepts_input = false;
	wlr_scene_node_set_enabled(&alarmrect->node, false);
	/* dmabuf feedback lets fullscreen apps skip compositing (direct scanout) */
	if (wlr_renderer_get_texture_formats(drw, WLR_BUFFER_CAP_DMABUF)) {
		wlr_drm_create(dpy, drw);
		wlr_scene_set_linux_dmabuf_v1(scene, wlr_linux_dmabuf_v1_create_with_renderer(dpy, 5, drw));
	}
	/* explicit sync: without it NVIDIA shows GPU-drawn windows (kitty, browsers)
	 * before they're finished, so they flicker */
	if (drw->features.timeline && backend->features.timeline)
		wlr_linux_drm_syncobj_manager_v1_create(dpy, 1, wlr_renderer_get_drm_fd(drw));
	wlr_log(WLR_INFO, "explicit sync: %s",
		drw->features.timeline && backend->features.timeline ? "on" : "unsupported here");

	compositor = wlr_compositor_create(dpy, 6, drw);
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
	relptr = wlr_relative_pointer_manager_v1_create(dpy);
	constraints = wlr_pointer_constraints_v1_create(dpy);
	wl_signal_add(&constraints->events.new_constraint, &new_constraint);
	tearingmgr = wlr_tearing_control_manager_v1_create(dpy, 1);
	idlenotifier = wlr_idle_notifier_v1_create(dpy);
	wl_signal_add(&wlr_idle_inhibit_v1_create(dpy)->events.new_inhibitor, &new_inhibitor);
	wl_signal_add(&wlr_session_lock_manager_v1_create(dpy)->events.new_lock, &new_lock);
	/* covers every screen, and clicks, until the locker draws */
	lockbg = wlr_scene_rect_create(layers[LyrLock], 65535, 65535, (float[4]){ 0, 0, 0, 1 });
	wlr_scene_node_set_position(&lockbg->node, -32768, -32768);
	wlr_scene_node_set_enabled(&lockbg->node, false);

	output_layout = wlr_output_layout_create(dpy);
	wl_signal_add(&output_layout->events.change, &layout_change);
	wl_signal_add(&backend->events.new_output, &new_output);
	wlr_xdg_output_manager_v1_create(dpy, output_layout);
	scene_layout = wlr_scene_attach_output_layout(scene, output_layout);
	wl_list_init(&mons);

	wl_list_init(&clients);
	wl_list_init(&tiles);
	wl_list_init(&ghosts);
	wl_list_init(&orphans);
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
	wl_signal_add(&cursor->events.swipe_update, &cursor_swipe);
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

	/* X11 apps; Xwayland itself starts only when the first one connects */
	if ((xwayland = wlr_xwayland_create(dpy, compositor, true))) {
		wl_signal_add(&xwayland->events.ready, &xwayland_ready);
		wl_signal_add(&xwayland->events.new_surface, &new_xsurface);
		setenv("DISPLAY", xwayland->display_name, 1);
	} else {
		wlr_log(WLR_ERROR, "couldn't set up XWayland: X11 apps won't open");
		unsetenv("DISPLAY");
	}
}

static void
run(const char *startup)
{
	const char *socket = wl_display_add_socket_auto(dpy), *dirs = getenv("XDG_CONFIG_DIRS");
	char buf[4096];

	if (!socket)
		die("couldn't create socket");
	setenv("WAYLAND_DISPLAY", socket, 1);
	/* every foot started here finds the ink theme, unless the user has a foot.ini */
	snprintf(buf, sizeof(buf), PREFIX "/share/dfwc/xdg:%s", dirs && *dirs ? dirs : "/etc/xdg");
	setenv("XDG_CONFIG_DIRS", buf, 1);
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
		&cursor_motion, &cursor_motion_absolute, &cursor_button, &cursor_axis, &cursor_frame, &cursor_swipe,
		&request_cursor, &request_sel, &request_psel, &request_shape, &layout_change,
		&new_layer, &new_deco, &request_activate, &new_constraint, &new_lock, &new_inhibitor };
	Monitor *m;
	Ghost *g, *gtmp;
	Workspace *w, *wtmp;

	wl_display_destroy_clients(dpy);
	if (xwayland) {
		wl_list_remove(&xwayland_ready.link);
		wl_list_remove(&new_xsurface.link);
		wlr_xwayland_destroy(xwayland);
	}
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
	if (wallimg)
		pixman_image_unref(wallimg);
	fcft_fini();
	for (size_t i = 0; i < LENGTH(listeners); i++)
		wl_list_remove(&listeners[i]->link);
	wl_list_for_each_safe(g, gtmp, &ghosts, link) /* their trees go with the scene */
		free(g);
	/* the scene takes the wallpapers and workspace trees with it; destroymon
	 * must not free them again */
	wl_list_for_each(m, &mons, link) {
		m->wall = NULL;
		wl_list_for_each_safe(w, wtmp, &m->spaces, link)
			free(w);
		wl_list_init(&m->spaces);
	}
	wl_list_for_each_safe(w, wtmp, &orphans, link)
		free(w);
	wl_list_init(&orphans);
	selws = NULL;
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

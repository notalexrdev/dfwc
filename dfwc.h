/* See LICENSE file for copyright and license details. */
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>
#include <xkbcommon/xkbcommon.h>

/* binds use this bit for "Mod"; it is replaced by modkey when matching */
#define MODKEY_BIT (1u << 31)

/* everything dfwc.set{} can change; empty strings mean "use the environment" */
typedef struct {
	char xkb_layout[64], xkb_variant[64], xkb_options[256], xkb_model[64];
	int repeat_rate, repeat_delay;
	bool touchpad_tap, touchpad_natural_scroll, touchpad_dwt;
	double touchpad_speed;
	char cursor_theme[64];
	int cursor_size;
	float background[4]; /* straight RGBA, like every color here */
	double scale;
	uint32_t modkey;     /* 0: Super, or Alt when nested */
	int border_width[4]; /* top, right, bottom, left */
	float border_focused[2][4], border_unfocused[2][4]; /* from, to: the same when solid */
	int border_angle;    /* gradient direction in degrees: 0 left to right, 90 top down */
	double border_spin;  /* seconds per turn of a gradient border; 0: still */
	int inner_border_width, border_gap;
	float inner_border_focused[4], inner_border_unfocused[4];
	int border_corners;  /* 0: full border, else length of corner-only brackets */
	int corner_radius;
	double opacity;
	bool error_alarm_fullscreen;
	char launcher[256];     /* runs after drawing a box */
	char restore_menu[256]; /* reads "id title" lines, prints the picked one */
	int box_timeout;        /* ms a drawn box waits for its window */
	int snap;               /* px within which moved windows snap to edges */
	/* tiling; these four keep their runtime value across reloads unless
	 * the config's own value for them changes */
	bool tiling;
	char layout[64];
	double mfact;
	int nmaster;
	int gaps_inner, gaps_outer;
	bool smart_gaps;        /* a lone tiled window gets no gaps and no borders */
	char tiled_drag[8];     /* moving a tiled window: "swap", "snap" or "float" */
	char wallpaper[256];    /* built-in name ("girl", "matrix", "hands"), a PNG path, or "" */
	int shadow_size;        /* blur radius of the shadow under floating windows; 0: none */
	int shadow_offset[2];   /* x, y: down and right by default, like a light from the top left */
	float shadow_color[4];
	bool blur;              /* blur the wallpaper behind see-through windows */
	int blur_radius, blur_passes;
	int anim_open, anim_close, anim_focus, anim_move; /* ms; 0: off */
	bool tearing;           /* fullscreen apps that ask may skip vsync for less input lag */
	float anim_curve[4];    /* cubic-bezier x1 y1 x2 y2, like CSS */
} Settings;

/* what a dfwc.style() or dfwc.rule() function sees */
typedef struct {
	const char *app_id, *title;
	bool focused, maximized, fullscreen, ontop;
} WinInfo;

typedef struct {
	float border[2][4], inner_border[4];
	double opacity;
} Style;

typedef struct {
	bool floating, ontop, maximized, fullscreen;
	int width, height; /* 0: the app's own size */
	bool pos;          /* x, y of the usable area instead of centering */
	int x, y;
} Rule; /* how a window starts, from dfwc.rule() */

enum { BindNone, BindDone, BindRepeat }; /* config_key() results */

/* dfwc.c */
extern uint32_t modkey;
void applysettings(void);
void arrange(void);
void configerror(const char *msg);
void focusstack(int dir);
void killclient(void);
void moveresize(int dx, int dy, int dw, int dh);
void quit(void);
void spawn(const char *cmd);
void minimize(void);
void togglefloat(void);
void zoom(void);
void restoremenu(void);
void togglefullscreen(void);
void togglemaximize(void);
void toggleontop(void);
void viewws(int n, int dir);              /* the n-th workspace, or dir steps from the shown one */
void sendws(int n, int dir, int follow);  /* the focused window there, maybe following it */

/* lua.c */
extern Settings settings;
void config_init(struct wl_event_loop *loop);
int config_key(uint32_t mods, xkb_keysym_t sym);
void config_reload(void);
void config_finish(void);
const Settings *config_own(void); /* what the config file itself set */
bool config_hasstyle(void);
bool config_haslayout(const char *name);
bool config_layout(const char *name, struct wlr_box area, int n, struct wlr_box *out);
void config_style(const WinInfo *w, Style *st);
void config_rule(const WinInfo *w, Rule *r);

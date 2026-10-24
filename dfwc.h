/* See LICENSE file for copyright and license details. */
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
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
	float border_focused[4], border_unfocused[4];
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
} Settings;

/* what a dfwc.style() function sees and may change */
typedef struct {
	const char *app_id, *title;
	bool focused, maximized, fullscreen;
} WinInfo;

typedef struct {
	float border[4], inner_border[4];
	double opacity;
} Style;

enum { BindNone, BindDone, BindRepeat }; /* config_key() results */

/* dfwc.c */
extern uint32_t modkey;
void applysettings(void);
void configerror(const char *msg);
void focusstack(int dir);
void killclient(void);
void moveresize(int dx, int dy, int dw, int dh);
void quit(void);
void spawn(const char *cmd);
void minimize(void);
void restoremenu(void);
void togglefullscreen(void);
void togglemaximize(void);
void toggleontop(void);

/* lua.c */
extern Settings settings;
void config_init(struct wl_event_loop *loop);
int config_key(uint32_t mods, xkb_keysym_t sym);
void config_reload(void);
void config_finish(void);
bool config_hasstyle(void);
void config_style(const WinInfo *w, Style *st);

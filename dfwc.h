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
	float background[4]; /* straight RGBA */
	double scale;
	uint32_t modkey;     /* 0: Super, or Alt when nested */
} Settings;

/* dfwc.c */
extern uint32_t modkey;
void applysettings(void);
void killclient(void);
void quit(void);
void spawn(const char *cmd);

/* lua.c */
extern Settings settings;
void config_init(struct wl_event_loop *loop);
bool config_key(uint32_t mods, xkb_keysym_t sym);
void config_reload(void);
void config_finish(void);

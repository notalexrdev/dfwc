/* See LICENSE file for copyright and license details. */
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

/* binds use this bit for "Mod"; it is replaced by modkey when matching */
#define MODKEY_BIT (1u << 31)

/* dfwc.c */
extern uint32_t modkey;
void killclient(void);
void quit(void);
void spawn(const char *cmd);

/* lua.c */
void config_init(struct wl_event_loop *loop);
bool config_key(uint32_t mods, xkb_keysym_t sym);
void config_reload(void);
void config_finish(void);

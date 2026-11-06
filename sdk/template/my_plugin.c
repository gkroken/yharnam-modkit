/* SPDX-License-Identifier: GPL-3.0-or-later
 * my_plugin: a starting point. Press F6 in the world to see your HP, where you stand and which
 * way you face. Another key: plugins.ini, [my_plugin] key = 64 (an SDL scancode: F1 is 58, so 64
 * is F7).
 *
 * Every plugin has the same pieces:
 *   - bb_plugin_info: its name and API version, read before anything runs;
 *   - bb_plugin_init: runs first (log and config work there);
 *   - bb_plugin_image: the game is loaded; register callbacks here, hooks only here;
 *   - callbacks such as on_frame, run on the game's main thread, the one place to change the game.
 * Rename it in bb_plugin_info and in CMakeLists.txt. The API is include/bbhost_plugin.h. */
#include "bbhost_plugin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static const BbHostApi *g_api;
static uint16_t g_key = 63; /* SDL scancode 63: F6 */
static uint8_t g_was_down;

static void on_frame(void *user) {
    (void)user;
    BbInput in;
    if (g_api->input(&in) != 0) return; /* no window */
    const uint8_t down = in.key[g_key];
    const int pressed = down && !g_was_down;
    g_was_down = down;
    if (!pressed || !in.playing) return;

    int64_t hp = 0, max_hp = 0;
    float xyz[3], yaw;
    if (g_api->player_stat_get("hp", &hp) != 0 || g_api->player_stat_get("max_hp", &max_hp) != 0 ||
        g_api->player_position(xyz, &yaw) != 0)
        return; /* no character loaded */
    char text[160];
    snprintf(text, sizeof text, "HP %lld / %lld at (%.1f, %.1f, %.1f) facing %.0f degrees", (long long)hp,
             (long long)max_hp, xyz[0], xyz[1], xyz[2], yaw * 180.0 / M_PI); /* yaw is in radians */
    g_api->show_message(text, 3.0f);
    g_api->log("%s", text);
}

BB_PLUGIN_EXPORT const BbPluginInfo bb_plugin_info = {
    BB_PLUGIN_API_VERSION, 0, "my_plugin", "My plugin", "0.1.0", "you",
    "Shows your HP, position and facing when you press a key.",
};

BB_PLUGIN_EXPORT int bb_plugin_init(const BbHostApi *api) {
    g_api = api;
    /* Refuse an older host that lacks what this plugin calls (input, API 13, is the newest call here). */
    if (api->size < offsetof(BbHostApi, input) + sizeof api->input) {
        api->log("this host's plugin API (%u) is too old", api->version);
        return 1; /* nonzero: the host unloads this plugin */
    }
    const char *key = api->config("my_plugin.key");
    if (*key) {
        const long value = strtol(key, NULL, 10);
        if (value > 0 && value < 512) g_key = (uint16_t)value;
    }
    return 0;
}

BB_PLUGIN_EXPORT int bb_plugin_image(const BbHostApi *api) {
    if (!api->eboot_is_109()) {
        api->log("not the 1.09 game: staying off");
        return 1;
    }
    return api->on_frame(on_frame, NULL);
}

/* SPDX-License-Identifier: GPL-3.0-or-later
 * flag_buff: gives the player a buff each time a chosen event flag turns on. It shows the game-
 * state API's pieces working together:
 *   - on_event_flag, registered in bb_plugin_image (the watcher is a hook on the game's setter);
 *   - sp_effect_apply on the main player, refused by the host for an id with no SpEffectParam row;
 *   - show_message over the game;
 *   - options from plugins.ini.
 *
 *   [flag_buff]
 *   flag = 11000000    ; the event flag to watch: required (no default, so nothing fires unasked)
 *   effect = 2200      ; the SpEffectParam row to apply (default 2200: a 60-second consumable effect)
 *   message = 1        ; 0: no banner
 *
 * Flag ids and effect ids are the game's own; the dump's params list the effects (SpEffectParam)
 * and the event scripts the flags. Build: bash tools/build-plugins.sh --install */
#include "bbhost_plugin.h"

#include <stdio.h>
#include <stdlib.h>

static const BbHostApi *g_api;
static uint32_t g_flag;
static int32_t g_effect = 2200;
static int g_message = 1;
static int g_given;

/* On the main thread, at the start of a frame: the changes since the last one, oldest first. */
static void on_flag(uint32_t id, int value, void *user) {
    (void)user;
    if (id != g_flag || !value) return;
    const int applied = g_api->sp_effect_apply(NULL, g_effect);  /* NULL: the main player */
    ++g_given;
    g_api->log("flag %u turned on: SpEffect %d applied = %d (%d so far)", id, g_effect, applied, g_given);
    if (g_message && applied == 0) {
        char text[96];
        snprintf(text, sizeof text, "Flag %u: a gift of SpEffect %d", id, g_effect);
        g_api->show_message(text, 3.0f);
    }
}

static long option(const char *key, long fallback) {
    const char *v = g_api->config(key);
    return *v ? strtol(v, NULL, 0) : fallback;
}

BB_PLUGIN_EXPORT const BbPluginInfo bb_plugin_info = {
    BB_PLUGIN_API_VERSION, BB_PLUGIN_GAMEPLAY, "flag_buff", "Flag buff", "1.0.0", "moding_tool",
    "Applies a SpEffect to the player each time a chosen event flag turns on.",
};

BB_PLUGIN_EXPORT int bb_plugin_image(const BbHostApi *api) {
    g_api = api;
    if (!api->eboot_is_109()) return 1;
    g_flag = (uint32_t)option("flag_buff.flag", 0);
    if (!g_flag) {
        api->log("no flag chosen: set [flag_buff] flag = <id> in plugins.ini");
        return 1;  /* unloads: nothing to do */
    }
    g_effect = (int32_t)option("flag_buff.effect", 2200);
    g_message = (int)option("flag_buff.message", 1);
    if (api->on_event_flag(on_flag, NULL)) {
        api->log("on_event_flag refused: no flag watching this run");
        return 1;
    }
    api->log("watching flag %u; it gives SpEffect %d", g_flag, g_effect);
    return 0;
}

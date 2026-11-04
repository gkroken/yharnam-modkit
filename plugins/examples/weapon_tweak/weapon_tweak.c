/* SPDX-License-Identifier: GPL-3.0-or-later
 * weapon_tweak: changes a param row by field name, as a balance mod would. The game's tables load
 * during its start, after bb_plugin_image, so the change is made from on_frame on the first frame
 * the row exists, then the plugin stops looking.
 *
 *   [weapon_tweak]
 *   table = EquipParamWeapon   ; any table: EquipParamProtector, SpEffectParam, NpcParam, ...
 *   row = 2000000              ; the row id
 *   field = attackBasePhysics  ; the game's own field name (port/out/paramdefs.txt lists them)
 *   value = 150                ; the new value, as text
 *
 * The defaults raise row 2000000's physical attack from the dump's 92 to 150. The log shows the
 * value before and after; param_set refuses a value outside the field's range. A character
 * holding the weapon keeps the numbers it already computed until it re-equips (or the next load).
 * Build: bash tools/build-plugins.sh --install */
#include "bbhost_plugin.h"

#include <stdlib.h>
#include <string.h>

static const BbHostApi *g_api;
static char g_table[64] = "EquipParamWeapon", g_field[64] = "attackBasePhysics", g_value[64] = "150";
static uint32_t g_row = 2000000;
static int g_done;
static unsigned g_frames;

static void text_option(const char *key, char *out, size_t n) {
    const char *v = g_api->config(key);
    if (*v) {
        strncpy(out, v, n - 1);
        out[n - 1] = 0;
    }
}

static void on_frame(void *user) {
    (void)user;
    if (g_done) return;
    if (++g_frames > 30 * 60 * 5) {  /* five minutes at 30 FPS without the table: give up */
        g_done = 1;
        g_api->log("%s row %u never appeared", g_table, g_row);
        return;
    }
    size_t bytes = 0;
    if (!g_api->param_row(g_table, g_row, &bytes)) return;  /* not loaded yet */
    g_done = 1;
    char before[64] = "?", after[64] = "?";
    g_api->param_get(g_table, g_row, g_field, before, sizeof before);
    const int set = g_api->param_set(g_table, g_row, g_field, g_value);
    g_api->param_get(g_table, g_row, g_field, after, sizeof after);
    g_api->log("%s row %u (%zu bytes): %s %s -> %s (param_set = %d)", g_table, g_row, bytes, g_field, before, after, set);
}

BB_PLUGIN_EXPORT const BbPluginInfo bb_plugin_info = {
    BB_PLUGIN_API_VERSION, BB_PLUGIN_GAMEPLAY, "weapon_tweak", "Weapon tweak", "1.0.0", "moding_tool",
    "Changes one field of one param row by name: a starting point for balance mods.",
};

BB_PLUGIN_EXPORT int bb_plugin_image(const BbHostApi *api) {
    g_api = api;
    if (!api->eboot_is_109()) return 1;
    text_option("weapon_tweak.table", g_table, sizeof g_table);
    text_option("weapon_tweak.field", g_field, sizeof g_field);
    text_option("weapon_tweak.value", g_value, sizeof g_value);
    const char *row = api->config("weapon_tweak.row");
    if (*row) g_row = (uint32_t)strtoul(row, NULL, 0);
    return api->on_frame(on_frame, NULL);
}

/* SPDX-License-Identifier: GPL-3.0-or-later
 * hello: the smallest useful plugin. It shows the pieces every plugin has:
 *   - bb_plugin_info, so the host can name it and decide whether to load it;
 *   - bb_plugin_init, which runs first (log and config work there);
 *   - bb_plugin_image, where the game's image can be used and callbacks are registered;
 *   - an on_frame callback, run on the game's main thread once a frame.
 *
 * It logs a line every `interval` seconds since the game started (plugins.ini: [hello]
 * interval = 10).
 * Build: bash tools/build-plugins.sh --install  (then start the game and read last_run.log). */
#include "bbhost_plugin.h"

#include <stdlib.h>
#include <windows.h>

static const BbHostApi *g_api;
static unsigned long long g_frames;
static double g_interval = 10.0, g_next = 10.0, g_seconds;

/* The game's frame rate varies (30, 60, uncapped), so count time, not frames: the host's tick
 * has no time argument, so this uses the performance counter. */
static LARGE_INTEGER g_start, g_frequency;

static void on_frame(void *user) {
    (void)user;
    ++g_frames;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_seconds = (double)(now.QuadPart - g_start.QuadPart) / (double)g_frequency.QuadPart;
    if (g_seconds >= g_next) {
        g_api->log("%.0f s, %llu frames (%.1f a second)", g_seconds, g_frames, g_frames / g_seconds);
        g_next += g_interval;
    }
}

BB_PLUGIN_EXPORT const BbPluginInfo bb_plugin_info = {
    BB_PLUGIN_API_VERSION, 0, "hello", "Hello", "1.0.0", "moding_tool",
    "Logs the time since start and the frame rate every few seconds: a starting point for plugins.",
};

BB_PLUGIN_EXPORT int bb_plugin_init(const BbHostApi *api) {
    g_api = api;
    /* A newer host may have more functions; an older one fewer. Check before relying on one. */
    if (api->version < 2 || api->size < offsetof(BbHostApi, on_frame) + sizeof api->on_frame) {
        api->log("this host's API (%u) has no on_frame", api->version);
        return 1;  /* nonzero: the host unloads this plugin */
    }
    const char *interval = api->config("hello.interval");
    if (*interval) {
        const double value = atof(interval);
        if (value >= 1.0) g_interval = g_next = value;
    }
    api->log("hello from %s (plugin API %u), every %.0f s", *api->config("host.name") ? api->config("host.name") : "bbhost",
             api->version, g_interval);
    return 0;
}

BB_PLUGIN_EXPORT int bb_plugin_image(const BbHostApi *api) {
    /* Addresses mean the 1.09 build only. This plugin uses none, but says what it found. */
    api->log("game build 1.09: %s", api->eboot_is_109() ? "yes" : "no");
    QueryPerformanceFrequency(&g_frequency);
    QueryPerformanceCounter(&g_start);
    return api->on_frame(on_frame, NULL);
}

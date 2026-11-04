/* SPDX-License-Identifier: GPL-3.0-or-later
 * frame_hook: hooks a game function and patches game bytes, the two things most plugins do.
 *   - hook2 on SprjFlipper::Update (BN 0x2434770), the game's frame-time update, run once a
 *     frame. Its 17-byte prologue (push rbp ... sub rsp, 0x38) is whole instructions, none of
 *     them RIP-relative, as hooks need. The callback counts calls, and on_frame logs the count
 *     next to the frame count every 300 frames (they should match).
 *   - With [frame_hook] skip_intro = 1 in plugins.ini, patch() empties the three logo movie
 *     names (UTF-16 "Lo..." -> "") that the community "Skip Intro" patch (by illusion,
 *     patches/Bloodborne.xml) zeroes. patch() checks the original bytes first: on another build,
 *     or if something else changed them, it refuses instead of writing.
 * Build: bash tools/build-plugins.sh --install */
#include "bbhost_plugin.h"

#include <stdlib.h>

static const BbHostApi *g_api;
static volatile unsigned long long g_calls;
static unsigned long long g_frames;

static int on_update(BbHookCtx *ctx, void *user) {
    (void)ctx, (void)user;
    ++g_calls;  /* runs on the game's main thread, inside its frame: keep hooks this short */
    return 0;   /* 0: let the function run (nonzero would skip it) */
}

static void on_frame(void *user) {
    (void)user;
    if (++g_frames % 300 == 0) g_api->log("frame %llu: SprjFlipper::Update ran %llu times", g_frames, g_calls);
}

BB_PLUGIN_EXPORT const BbPluginInfo bb_plugin_info = {
    BB_PLUGIN_API_VERSION, 0, "frame_hook", "Frame hook", "1.0.0", "moding_tool",
    "Hooks the game's frame update and counts it; optionally skips the intro logos with patch().",
};

BB_PLUGIN_EXPORT int bb_plugin_image(const BbHostApi *api) {
    g_api = api;
    if (!api->eboot_is_109()) {
        api->log("not the 1.09 build: the addresses here mean nothing on it");
        return 1;
    }
    static const unsigned char prologue[17] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                               0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x38};
    const int hooked = api->hook2(0x2434770, prologue, sizeof prologue, on_update, NULL);
    api->log("hook2(SprjFlipper::Update) = %d (0 placed, 1 other bytes, 2 not placeable)", hooked);

    if (atoi(api->config("frame_hook.skip_intro")) == 1) {
        static const unsigned char lo[4] = {'L', 0, 'o', 0}, empty[4] = {0, 0, 0, 0};
        static const uint64_t names[3] = {0x4d99138, 0x4d99154, 0x4d9916e};
        for (int i = 0; i < 3; ++i)
            api->log("skip intro: patch(0x%llx) = %d", (unsigned long long)names[i],
                     api->patch(names[i], lo, empty, sizeof lo));
    }
    return hooked == 0 ? api->on_frame(on_frame, NULL) : 1;
}

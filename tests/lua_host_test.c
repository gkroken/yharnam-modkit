/* SPDX-License-Identifier: GPL-3.0-or-later
 * bb_lua.dll against a fake host: the scripts load in name order, run once a frame, read their
 * options, and only reach what the sandbox leaves; a script that errors, loops forever, eats
 * memory or doesn't parse is stopped alone (log line and banner) while the others keep running.
 * Usage: lua-host-test <path to bb_lua.dll> */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "bbhost_plugin.h"

static int checks;
#define CHECK(cond)                                                                              \
    do {                                                                                         \
        ++checks;                                                                                \
        if (!(cond)) {                                                                           \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);             \
            exit(1);                                                                             \
        }                                                                                        \
    } while (0)

/* --- the fake host ------------------------------------------------------------------------------ */

static char g_log[64 * 1024];
static char g_banner[256];
static int g_banners;
static void (*g_frame)(void *);
static void *g_frame_user;
static char g_dir[MAX_PATH];

static void host_log(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    strncat(g_log, line, sizeof g_log - strlen(g_log) - 2);
    strcat(g_log, "\n");
    printf("  log: %s\n", line);
}
static const char *host_config(const char *key) {
    if (!strcmp(key, "a_good.greeting")) return "hello from plugins.ini";
    return "";
}
static const char *host_plugin_dir(const char *name) {
    static char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s\\%s", g_dir, name);
    return dir;
}
static int host_on_frame(void (*fn)(void *), void *user) {
    g_frame = fn;
    g_frame_user = user;
    return 0;
}
static int host_show_message(const char *text, float seconds) {
    (void)seconds;
    snprintf(g_banner, sizeof g_banner, "%s", text);
    ++g_banners;
    return 0;
}
static int host_is_109(void) { return 1; }

static int count(const char *needle) {
    int n = 0;
    for (const char *p = g_log; (p = strstr(p, needle)) != NULL; p += strlen(needle)) ++n;
    return n;
}

static void write_script(const char *name, const char *text) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\lua\\%s", g_dir, name);
    FILE *f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: lua-host-test <bb_lua.dll>\n");
        return 2;
    }
    GetTempPathA(MAX_PATH, g_dir);
    snprintf(g_dir + strlen(g_dir), MAX_PATH - strlen(g_dir), "bb-lua-test-%lu", GetCurrentProcessId());
    CreateDirectoryA(g_dir, NULL);
    char lua_dir[MAX_PATH];
    snprintf(lua_dir, sizeof lua_dir, "%s\\lua", g_dir);
    CreateDirectoryA(lua_dir, NULL);

    write_script("a_good.lua",
                 "local frames = 0\n"
                 "bb.log('greeting:', bb.config('greeting'), bb.config('missing'))\n"
                 "bb.on_frame(function() frames = frames + 1; if frames == 3 then print('three frames', frames) end end)\n");
    write_script("b_errors.lua",
                 "local n = 0\n"
                 "bb.on_frame(function() n = n + 1; if n == 2 then error('boom at frame two') end end)\n");
    write_script("c_loops.lua", "bb.on_frame(function() while true do end end)\n");
    write_script("d_memory.lua",
                 "bb.on_frame(function() local t = {} for i = 1, 1e9 do t[i] = string.rep('x', 4096) .. i end end)\n");
    write_script("e_syntax.lua", "bb.on_frame(function( end\n");
    write_script("f_sandbox.lua",
                 "local reached = {}\n"
                 "for _, name in ipairs({'io', 'require', 'load', 'loadfile', 'dofile', 'debug', 'package'}) do\n"
                 "  if _G[name] ~= nil then reached[#reached + 1] = name end\n"
                 "end\n"
                 "if string.dump then reached[#reached + 1] = 'string.dump' end\n"
                 "if os.execute or os.remove or os.getenv then reached[#reached + 1] = 'os extras' end\n"
                 "if not (os.clock and os.time and os.date and math.floor and table.insert and utf8.char\n"
                 "        and coroutine.wrap and string.format) then reached[#reached + 1] = 'a needed library is missing' end\n"
                 "bb.log('sandbox: ' .. (#reached == 0 and 'closed' or ('reaches ' .. table.concat(reached, ','))))\n");
    write_script("readme.txt", "not a script");

    HMODULE dll = LoadLibraryA(argv[1]);
    CHECK(dll);
    const BbPluginInfo *info = (const BbPluginInfo *)(void *)GetProcAddress(dll, "bb_plugin_info");
    int (*image)(const BbHostApi *) = (int (*)(const BbHostApi *))(void *)GetProcAddress(dll, "bb_plugin_image");
    CHECK(info && image && !strcmp(info->name, "lua"));

    BbHostApi api;
    memset(&api, 0, sizeof api);
    api.version = BB_PLUGIN_API_VERSION;
    api.size = sizeof api;
    api.log = host_log;
    api.config = host_config;
    api.plugin_dir = host_plugin_dir;
    api.on_frame = host_on_frame;
    api.show_message = host_show_message;
    api.eboot_is_109 = host_is_109;

    CHECK(image(&api) == 0 && g_frame);
    /* Loading: the good ones loaded, the syntax error stopped at load, in name order. */
    CHECK(strstr(g_log, "a_good: greeting: hello from plugins.ini nil"));
    CHECK(strstr(g_log, "a_good: loaded") && strstr(g_log, "b_errors: loaded") && strstr(g_log, "f_sandbox: loaded"));
    CHECK(strstr(g_log, "e_syntax: stopped:") && !strstr(g_log, "e_syntax: loaded"));
    CHECK(strstr(g_log, "a_good: loaded") < strstr(g_log, "b_errors: loaded"));
    CHECK(strstr(g_log, "f_sandbox: sandbox: closed"));
    CHECK(!strstr(g_log, "readme"));
    CHECK(strstr(g_log, "5 script(s) running, 1 stopped"));

    for (int i = 0; i < 5; ++i) g_frame(g_frame_user);
    /* The good script ran every frame; each bad one stopped alone, with a reason. */
    CHECK(count("a_good: three frames 3") == 1);
    CHECK(strstr(g_log, "b_errors: stopped:") && strstr(g_log, "boom at frame two"));
    CHECK(strstr(g_log, "c_loops: stopped:") && strstr(g_log, "took too long"));
    CHECK(strstr(g_log, "d_memory: stopped:") && strstr(g_log, "not enough memory"));
    CHECK(count("b_errors: stopped:") == 1);  /* stopped once, not every frame */
    CHECK(g_banners == 4 && strstr(g_banner, "stopped"));  /* syntax, errors, loops, memory */

    /* No scripts: the plugin unloads itself (returns nonzero). */
    printf("lua host: %d checks passed\n", checks);
    return 0;
}

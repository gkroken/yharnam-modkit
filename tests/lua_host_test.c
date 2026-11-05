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
static BbHostApi g_api;

/* Two mod names over 64 bytes, the same for their first 67 (tool-51d16fc-F1): one disabled, one not. */
#define LONG_MOD "A_very_long_mod_name_that_goes_well_past_sixty_four_bytes_of_text__"
#define LONG_OFF LONG_MOD "off"
#define LONG_ON LONG_MOD "on"


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
    if (!strcmp(key, "alpha/main.colour")) return "red";
    if (!strcmp(key, LONG_ON "/main.colour")) return "green";
    return "";
}
static char g_mods[MAX_PATH];
static const char *host_mods_dir(void) { return g_mods; }
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

/* The game, faked: flags 1-99, one param row, a player, two characters, an effect list. */
static int g_flags[100];
static void (*g_flag_cb)(uint32_t, int, void *);
static void (*g_load_cb)(uint32_t, void *);
static char g_weapon_attack[16] = "92";
static int64_t g_echoes = 245;
static int g_effects[8], g_effect_count;
static void *g_effect_target;
static char g_text[128];
static float g_text_x;
static uint32_t g_text_align;
static int host_flag_get(uint32_t id, int *v) {
    if (id >= 100) return 1;
    *v = g_flags[id];
    return 0;
}
static int host_flag_set(uint32_t id, int v) {
    if (id >= 100) return 1;
    g_flags[id] = v;
    return 0;
}
static int host_on_flag(void (*fn)(uint32_t, int, void *), void *user) {
    (void)user;
    g_flag_cb = fn;
    return 0;
}
static int host_on_load(void (*fn)(uint32_t, void *), void *user) {
    (void)user;
    g_load_cb = fn;
    return 0;
}
static int host_param_get(const char *table, uint32_t id, const char *field, char *out, size_t n) {
    if (strcmp(table, "EquipParamWeapon") || id != 2000000 || strcmp(field, "attackBasePhysics")) return 1;
    snprintf(out, n, "%s", g_weapon_attack);
    return 0;
}
static int host_param_set(const char *table, uint32_t id, const char *field, const char *value) {
    if (strcmp(table, "EquipParamWeapon") || id != 2000000 || strcmp(field, "attackBasePhysics")) return 1;
    snprintf(g_weapon_attack, sizeof g_weapon_attack, "%s", value);
    return 0;
}
static int host_param_ids(const char *table, uint32_t *ids, size_t max, size_t *count) {
    if (strcmp(table, "EquipParamWeapon")) return 1;
    static const uint32_t all[3] = {1000, 2000000, 2000100};
    *count = 3;
    for (size_t i = 0; ids && i < max && i < 3; ++i) ids[i] = all[i];
    return 0;
}
static int host_stat_get(const char *name, int64_t *v) {
    if (strcmp(name, "echoes")) return 1;
    *v = g_echoes;
    return 0;
}
static int host_stat_set(const char *name, int64_t v) {
    if (strcmp(name, "echoes")) return 1;
    g_echoes = v;
    return 0;
}
static int host_position(float xyz[3], float *yaw) {
    xyz[0] = 1.5f, xyz[1] = -2.0f, xyz[2] = 3.25f;
    *yaw = 0.5f;
    return 0;
}
static int host_block(uint32_t *b) {
    *b = 0x18010000;
    return 0;
}
static int g_npc;
static int host_chr_list(BbChr *out, size_t max, size_t *count) {
    *count = 2;
    if (out && max >= 1) {
        memset(&out[0], 0, sizeof out[0]);
        out[0].ins = &g_echoes;
        out[0].is_player = 1;
        out[0].npc_param = -1;
        out[0].hp = 573;
    }
    if (out && max >= 2) {
        memset(&out[1], 0, sizeof out[1]);
        out[1].ins = &g_npc;
        out[1].npc_param = 256000;
        out[1].model = 2560;
        out[1].pos[0] = 7.5f;
    }
    return 0;
}
static int host_effect_apply(void *chr, int32_t id) {
    if (id <= 0 || g_effect_count == 8) return 1;
    g_effect_target = chr;
    g_effects[g_effect_count++] = id;
    return 0;
}
static int host_effect_has(void *chr, int32_t id) {
    for (int i = 0; i < g_effect_count; ++i)
        if (g_effects[i] == id && chr == g_effect_target) return 1;
    return 0;
}
static int host_show_text(const char *text, float seconds, float x, float y, float scale, uint32_t align) {
    (void)seconds, (void)y, (void)scale;
    snprintf(g_text, sizeof g_text, "%s", text);
    g_text_x = x;
    g_text_align = align;
    return 0;
}

static int count(const char *needle) {
    int n = 0;
    for (const char *p = g_log; (p = strstr(p, needle)) != NULL; p += strlen(needle)) ++n;
    return n;
}

/* One script alone in a fresh copy of the DLL (its own globals, its own Lua history): whether it
 * was stopped within its first frame. Run alone, the reviewer's pcall loop hangs a build without
 * the sticky budget; in the mixed run above it happened not to (tool-76b48eb-F1). */
static int stopped_alone(const char *dll, const char *tag, const char *text) {
    char copy[MAX_PATH], folder[MAX_PATH], path[MAX_PATH];
    snprintf(copy, sizeof copy, "%s\\bb_lua_%s.dll", g_dir, tag);
    if (!CopyFileA(dll, copy, FALSE)) return 0;
    snprintf(g_dir + strlen(g_dir), MAX_PATH - strlen(g_dir), "\\%s", tag);  /* plugin_dir now points here */
    CreateDirectoryA(g_dir, NULL);
    snprintf(folder, sizeof folder, "%s\\lua", g_dir);
    CreateDirectoryA(folder, NULL);
    snprintf(path, sizeof path, "%s\\%s.lua", folder, tag);
    FILE *f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
    g_log[0] = 0;
    g_frame = NULL;
    HMODULE m = LoadLibraryA(copy);
    int (*image)(const BbHostApi *) = m ? (int (*)(const BbHostApi *))(void *)GetProcAddress(m, "bb_plugin_image") : NULL;
    if (!image || image(&g_api) || !g_frame) return 0;
    g_frame(g_frame_user);  /* hangs (the test runner's timeout) if the script isn't stopped */
    char want[96];
    snprintf(want, sizeof want, "%s: stopped: took too long", tag);
    *strrchr(g_dir, '\\') = 0;  /* back to the test's folder */
    return strstr(g_log, want) != NULL;
}

/* A fresh copy of the DLL loaded with BB_MODS_CONFIG naming a file holding `json`: g_log has
 * what it logged. */
static int load_with_config(const char *dll, const char *tag, const char *json, size_t len) {
    char config[MAX_PATH], copy[MAX_PATH];
    snprintf(config, sizeof config, "%s\\%s-mods.json", g_dir, tag);
    FILE *f = fopen(config, "wb");
    fwrite(json, 1, len, f);
    fclose(f);
    SetEnvironmentVariableA("BB_MODS_CONFIG", config);
    snprintf(copy, sizeof copy, "%s\\bb_lua_%s.dll", g_dir, tag);
    if (!CopyFileA(dll, copy, FALSE)) return 0;
    g_log[0] = 0;
    HMODULE m = LoadLibraryA(copy);
    int (*image)(const BbHostApi *) = m ? (int (*)(const BbHostApi *))(void *)GetProcAddress(m, "bb_plugin_image") : NULL;
    const int ok = image && image(&g_api) == 0;
    SetEnvironmentVariableA("BB_MODS_CONFIG", NULL);
    return ok;
}

/* One script alone, in a fresh copy of the DLL with its own folder; g_log has what it logged. */
static int load_alone(const char *dll, const char *tag, const char *text) {
    char copy[MAX_PATH], folder[MAX_PATH], path[MAX_PATH];
    snprintf(copy, sizeof copy, "%s\\bb_lua_%s.dll", g_dir, tag);
    if (!CopyFileA(dll, copy, FALSE)) return 0;
    snprintf(g_dir + strlen(g_dir), MAX_PATH - strlen(g_dir), "\\%s", tag);
    CreateDirectoryA(g_dir, NULL);
    snprintf(folder, sizeof folder, "%s\\lua", g_dir);
    CreateDirectoryA(folder, NULL);
    snprintf(path, sizeof path, "%s\\%s.lua", folder, tag);
    FILE *f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
    g_log[0] = 0;
    HMODULE m = LoadLibraryA(copy);
    int (*image)(const BbHostApi *) = m ? (int (*)(const BbHostApi *))(void *)GetProcAddress(m, "bb_plugin_image") : NULL;
    const int ok = image && image(&g_api) == 0;
    *strrchr(g_dir, '\\') = 0;
    return ok;
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
    /* tool-711f289-F1: an endless loop that catches the budget error, a coroutine doing the same,
     * and a backtracking pattern (C(59,29) steps), which runs in C where the hook can't reach. */
    write_script("g_pcall_loop.lua", "bb.on_frame(function() while true do pcall(function() while true do end end) end end)\n");
    write_script("h_coroutine_loop.lua",
                 "bb.on_frame(function()\n"
                 "  while true do coroutine.resume(coroutine.create(function() while true do end end)) end\n"
                 "end)\n");
    write_script("i_pattern.lua",
                 "bb.on_frame(function() local s = ('a'):rep(30); s:find(('a*'):rep(30) .. 'b') end)\n");
    write_script("j_pattern_pcall.lua",
                 "bb.on_frame(function()\n"
                 "  local s = ('a'):rep(30)\n"
                 "  while true do pcall(string.find, s, ('a*'):rep(30) .. 'b') end\n"
                 "end)\n");
    write_script("k_recovers.lua",
                 "local n = 0\n"
                 "bb.on_frame(function()\n"
                 "  n = n + 1\n"
                 "  if n == 1 then pcall(function() while true do end end) end  -- caught once, then the call ends\n"
                 "  if n == 3 then bb.log('k still running at frame 3') end\n"
                 "end)\n");
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
    /* Every game binding, through the fake host. */
    write_script("m_game.lua",
                 "bb.log('flag 5', bb.flag.get(5), 'flag 500', bb.flag.get(500))\n"
                 "bb.log('set flag 5', bb.flag.set(5, true), bb.flag.get(5))\n"
                 "bb.log('attack', bb.param.get('EquipParamWeapon', 2000000, 'attackBasePhysics'),\n"
                 "       bb.param.set('EquipParamWeapon', 2000000, 'attackBasePhysics', 150),\n"
                 "       bb.param.get('EquipParamWeapon', 2000000, 'attackBasePhysics'),\n"
                 "       bb.param.get('NoParam', 1, 'x'))\n"
                 "local ids = bb.param.ids('EquipParamWeapon')\n"
                 "bb.log('ids', #ids, ids[1], ids[3], bb.param.ids('NoParam'))\n"
                 "bb.log('echoes', bb.player.stat('echoes'), bb.player.stat('echoes', 999), bb.player.stat('echoes'), bb.player.stat('mana'))\n"
                 "local x, y, z, yaw = bb.player.position()\n"
                 "bb.log('position', x, y, z, yaw, string.format('block 0x%08x', bb.player.block()))\n"
                 "local list = bb.chr.list()\n"
                 "bb.log('chr', #list, list[1].player, list[1].hp, list[2].player, list[2].npc, list[2].model, list[2].x)\n"
                 "bb.log('effect', bb.effect.apply(2200), bb.effect.has(2200), bb.effect.apply(2200, list[2].ins), bb.effect.has(2200, list[2].ins),\n"
                 "       bb.effect.apply(-1))\n"
                 "bb.log('text', bb.message('hi'), bb.text('placed', 3, 0.25, 0.5, 1, 'right bottom'))\n"
                 "bb.flag.on(function(id, on) bb.log('flag changed', id, on) end)\n"
                 "bb.on_world_load(function(block) bb.log(string.format('world load 0x%08x', block)) end)\n"
                 "local ok, err = pcall(bb.effect.apply, 2200, 'not a character')\n"
                 "bb.log('bad chr argument raises', not ok)\n"
                 "local _, named = pcall(bb.effect.has, 2200, 12345)\n"
                 "bb.log('error names', named)\n");

    /* Mods with scripts: two with a main.lua each, one disabled in mods.json, one without scripts. */
    snprintf(g_mods, sizeof g_mods, "%s\\mods", g_dir);
    CreateDirectoryA(g_mods, NULL);
    static const char *const mod_names[] = {"alpha", "beta", "gamma", "delta"};
    for (int i = 0; i < 4; ++i) {
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\%s", g_mods, mod_names[i]);
        CreateDirectoryA(p, NULL);
        if (i == 3) continue;  /* delta: no scripts folder */
        snprintf(p + strlen(p), sizeof p - strlen(p), "\\scripts");
        CreateDirectoryA(p, NULL);
        snprintf(p + strlen(p), sizeof p - strlen(p), "\\main.lua");
        FILE *f = fopen(p, "wb");
        fprintf(f, "bb.log('%s main running, colour', bb.config('colour'))\n", mod_names[i]);
        fclose(f);
    }
    {
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\mods.json", g_dir);
        FILE *f = fopen(p, "wb");
        /* A disabled name holding brackets and an escaped quote comes first: gamma must still be read. */
        fputs("{\"version\": 1, \"note\": \"]\", \"disabled\": [\"Cool Mod [v2] \\\"x\\\"\", \"gamma\", \"" LONG_OFF "\"],\n"
              " \"order\": [\"beta\", \"alpha\"]}", f);
        fclose(f);
    }
    for (int i = 0; i < 2; ++i) {  /* the long-named mods */
        const char *name = i ? LONG_ON : LONG_OFF;
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\%s", g_mods, name);
        CreateDirectoryA(p, NULL);
        snprintf(p + strlen(p), sizeof p - strlen(p), "\\scripts");
        CreateDirectoryA(p, NULL);
        snprintf(p + strlen(p), sizeof p - strlen(p), "\\main.lua");
        FILE *f = fopen(p, "wb");
        fprintf(f, "bb.log('long %s running, colour', bb.config('colour'))\n", i ? "on" : "off");
        fclose(f);
    }

    {
        /* A non-ASCII mod name (tool-009595e-F2): Blød, made with the wide API. */
        wchar_t wide[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, g_mods, -1, wide, MAX_PATH);
        wcscat(wide, L"\\Bl\u00f8d");
        CreateDirectoryW(wide, NULL);
        wcscat(wide, L"\\scripts");
        CreateDirectoryW(wide, NULL);
        wcscat(wide, L"\\main.lua");
        FILE *f = _wfopen(wide, L"wb");
        fputs("bb.log('the non-ASCII mod ran')\n", f);
        fclose(f);
    }

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
    api.event_flag_get = host_flag_get;
    api.event_flag_set = host_flag_set;
    api.on_event_flag = host_on_flag;
    api.on_world_load = host_on_load;
    api.param_get = host_param_get;
    api.param_set = host_param_set;
    api.param_ids = host_param_ids;
    api.player_stat_get = host_stat_get;
    api.player_stat_set = host_stat_set;
    api.player_position = host_position;
    api.player_block = host_block;
    api.chr_list = host_chr_list;
    api.sp_effect_apply = host_effect_apply;
    api.sp_effect_has = host_effect_has;
    api.show_text = host_show_text;
    api.mods_dir = host_mods_dir;

    CHECK(image(&api) == 0 && g_frame);
    /* Loading: the good ones loaded, the syntax error stopped at load, in name order. */
    CHECK(strstr(g_log, "a_good: greeting: hello from plugins.ini nil"));
    CHECK(strstr(g_log, "a_good: loaded") && strstr(g_log, "b_errors: loaded") && strstr(g_log, "f_sandbox: loaded"));
    CHECK(strstr(g_log, "e_syntax: stopped:") && !strstr(g_log, "e_syntax: loaded"));
    CHECK(strstr(g_log, "a_good: loaded") < strstr(g_log, "b_errors: loaded"));
    CHECK(strstr(g_log, "f_sandbox: sandbox: closed"));
    CHECK(!strstr(g_log, "readme"));
    CHECK(strstr(g_log, "15 script(s) running, 1 stopped"));
    /* tool-51d16fc-F1: names are compared whole. The disabled long one is left out, its twin runs
     * under its whole name, and reads its own plugins.ini section. */
    CHECK(!strstr(g_log, "long off running") && strstr(g_log, "mod " LONG_OFF " is disabled"));
    CHECK(strstr(g_log, LONG_ON "/main: long on running, colour green"));
    CHECK(strstr(g_log, "Bl\xc3\xb8""d/main: the non-ASCII mod ran"));     /* F2 */
    CHECK(strstr(g_log, "beta/main: loaded") < strstr(g_log, "alpha/main: loaded"));  /* the player's order (N1) */
    /* Mods' scripts: named "<mod>/<file>", their own plugins.ini section, a disabled mod left out. */
    CHECK(strstr(g_log, "alpha/main: alpha main running, colour red") && strstr(g_log, "beta/main: beta main running, colour nil"));
    CHECK(!strstr(g_log, "gamma main running") && strstr(g_log, "mod gamma is disabled"));
    CHECK(strstr(g_log, "f_sandbox: loaded") < strstr(g_log, "alpha/main: loaded"));  /* the user's scripts first */
    /* The game bindings, as the fake host saw them. */
    CHECK(strstr(g_log, "m_game: flag 5 false flag 500 nil"));
    CHECK(strstr(g_log, "m_game: set flag 5 true true") && g_flags[5] == 1);
    CHECK(strstr(g_log, "m_game: attack 92 true 150 nil") && !strcmp(g_weapon_attack, "150"));
    CHECK(strstr(g_log, "m_game: ids 3 1000 2000100 nil"));
    CHECK(strstr(g_log, "m_game: echoes 245 true 999 nil") && g_echoes == 999);
    CHECK(strstr(g_log, "m_game: position 1.5 -2.0 3.25 0.5 block 0x18010000"));
    CHECK(strstr(g_log, "m_game: chr 2 true 573 false 256000 2560 7.5"));
    CHECK(strstr(g_log, "m_game: effect true true true true false") && g_effect_target == &g_npc);
    CHECK(strstr(g_log, "m_game: text true true") && !strcmp(g_text, "placed") && g_text_x == 0.25f &&
          g_text_align == (BB_TEXT_RIGHT | BB_TEXT_BOTTOM));
    CHECK(strstr(g_log, "m_game: bad chr argument raises true"));
    CHECK(strstr(g_log, "bad argument #2 to 'bb.effect.has'"));  /* tool-9bb9566-N1 */
    CHECK(g_flag_cb && g_load_cb);

    for (int i = 0; i < 5; ++i) g_frame(g_frame_user);
    /* The good script ran every frame; each bad one stopped alone, with a reason. */
    CHECK(count("a_good: three frames 3") == 1);
    CHECK(strstr(g_log, "b_errors: stopped:") && strstr(g_log, "boom at frame two"));
    CHECK(strstr(g_log, "c_loops: stopped:") && strstr(g_log, "took too long"));
    CHECK(strstr(g_log, "d_memory: stopped:") && strstr(g_log, "not enough memory"));
    CHECK(count("b_errors: stopped:") == 1);  /* stopped once, not every frame */
    CHECK(strstr(g_log, "g_pcall_loop: stopped:") && strstr(g_log, "h_coroutine_loop: stopped:"));
    CHECK(strstr(g_log, "i_pattern: stopped:") && strstr(g_log, "j_pattern_pcall: stopped:"));
    /* The budget is per call and absolute: catching its error once doesn't buy more, the next
     * instruction outside the pcall raises again, so even this script stops. */
    CHECK(strstr(g_log, "k_recovers: stopped:") && !strstr(g_log, "k still running at frame 3"));
    CHECK(g_banners == 10 && strstr(g_banner, "stopped"));  /* 9 scripts stopped, and m_game's bb.message */

    /* The host's flag and world-load callbacks reach the script's functions. */
    g_flag_cb(77, 1, NULL);
    g_load_cb(0x18000000, NULL);
    CHECK(strstr(g_log, "m_game: flag changed 77 true") && strstr(g_log, "m_game: world load 0x18000000"));

    /* BB_MODS_CONFIG (the launcher's own mods.json, wherever it is) decides over the file beside
     * the mods folder: here it disables alpha (tool-009595e-F1). */
    {
        char config[MAX_PATH];
        snprintf(config, sizeof config, "%s\\elsewhere-mods.json", g_dir);
        FILE *f = fopen(config, "wb");
        fputs("{\"disabled\": [\"alpha\"]}", f);
        fclose(f);
        char env[MAX_PATH + 32];
        snprintf(env, sizeof env, "BB_MODS_CONFIG=%s", config);
        _putenv(env);
        SetEnvironmentVariableA("BB_MODS_CONFIG", config);
        char copy[MAX_PATH];
        snprintf(copy, sizeof copy, "%s\\bb_lua_config.dll", g_dir);
        CopyFileA(argv[1], copy, FALSE);
        g_log[0] = 0;
        g_api = api;
        HMODULE m = LoadLibraryA(copy);
        int (*image2)(const BbHostApi *) = (int (*)(const BbHostApi *))(void *)GetProcAddress(m, "bb_plugin_image");
        CHECK(image2 && image2(&g_api) == 0);
        CHECK(strstr(g_log, "mod alpha is disabled") && !strstr(g_log, "alpha main running"));
        CHECK(strstr(g_log, "gamma main running"));  /* that file doesn't disable gamma */
        _putenv("BB_MODS_CONFIG=");
        SetEnvironmentVariableA("BB_MODS_CONFIG", NULL);
    }

    /* tool-51d16fc-F1: a mods.json that can't be read whole loads no mod's scripts (the user's own
     * scripts still load), with a log line. */
    {
        static const char *const broken[] = {
            "{\"disabled\": [\"alpha\"",        /* cut off */
            "{\"disabled\": null}",              /* not a list: mods.py refuses it too */
            "{\"disabled\": [\"alpha\", 5]}",    /* not all names */
            "{\"disabled\": [\"al\\u00\"]}",     /* a bad escape */
            "{\"disabled\": [\"alpha\"]} trailing",
            "",
        };
        g_api = api;
        for (size_t i = 0; i < sizeof broken / sizeof *broken; ++i) {
            char tag[32];
            snprintf(tag, sizeof tag, "broken%zu", i);
            CHECK(load_with_config(argv[1], tag, broken[i], strlen(broken[i])));
            CHECK(strstr(g_log, "mods.json could not be read whole: no mod's scripts are loaded"));
            CHECK(!strstr(g_log, "main running") && !strstr(g_log, "/main: loaded"));
            CHECK(strstr(g_log, "a_good: loaded"));
        }
        /* No cap on the list: 1000 names, beta last, each longer than the old 64 bytes. */
        size_t cap = 1 << 20, len = 0;
        char *big = malloc(cap);
        len += (size_t)snprintf(big + len, cap - len, "{\"disabled\": [");
        for (int i = 0; i < 1000; ++i)
            len += (size_t)snprintf(big + len, cap - len, "\"%s%04d\", ", LONG_MOD, i);
        len += (size_t)snprintf(big + len, cap - len, "\"beta\"]}");
        CHECK(load_with_config(argv[1], "biglist", big, len));
        CHECK(strstr(g_log, "mod beta is disabled") && !strstr(g_log, "beta main running"));
        CHECK(strstr(g_log, "alpha main running") && strstr(g_log, "long off running"));
        free(big);
        /* No mods.json at all: every mod on, as mods.py has it. */
        char missing[MAX_PATH];
        snprintf(missing, sizeof missing, "%s\\no-such-mods.json", g_dir);
        SetEnvironmentVariableA("BB_MODS_CONFIG", missing);
        char copy[MAX_PATH];
        snprintf(copy, sizeof copy, "%s\\bb_lua_missing.dll", g_dir);
        CopyFileA(argv[1], copy, FALSE);
        g_log[0] = 0;
        HMODULE m = LoadLibraryA(copy);
        int (*image3)(const BbHostApi *) = (int (*)(const BbHostApi *))(void *)GetProcAddress(m, "bb_plugin_image");
        CHECK(image3 && image3(&g_api) == 0);
        CHECK(strstr(g_log, "gamma main running") && strstr(g_log, "long off running"));
        SetEnvironmentVariableA("BB_MODS_CONFIG", NULL);
    }

    /* tool-9614890-F1: online play (BB_ONLINE=1 without BB_COOP=1) refuses every call that changes
     * the game, logged once; reading and text still work. Co-op lets them through, as the host does. */
    {
        static const char *const writes =
            "bb.log('online writes', bb.flag.set(6, true), bb.param.set('EquipParamWeapon', 2000000, 'attackBasePhysics', 7),\n"
            "       bb.player.stat('echoes', 1), bb.effect.apply(2200))\n"
            "bb.log('online reads', bb.flag.get(6), bb.param.get('EquipParamWeapon', 2000000, 'attackBasePhysics'),\n"
            "       bb.player.stat('echoes'), bb.effect.has(2200), bb.text('still shown', 3, 0.5, 0.5))\n";
        g_api = api;
        g_flags[6] = 0;
        snprintf(g_weapon_attack, sizeof g_weapon_attack, "92");
        g_echoes = 245;
        g_effect_count = 0;
        const int effects_before = 0;
        SetEnvironmentVariableA("BB_ONLINE", "1");
        CHECK(load_alone(argv[1], "online", writes));
        CHECK(strstr(g_log, "online play: scripts run, but their calls that change the game do nothing"));
        CHECK(strstr(g_log, "online: online writes false false false false"));
        CHECK(g_flags[6] == 0 && !strcmp(g_weapon_attack, "92") && g_echoes == 245 && g_effect_count == effects_before);
        CHECK(strstr(g_log, "online: online reads false 92 245 false true"));
        CHECK(count("and every other call that changes the game do nothing") == 1);  /* once per script */
        SetEnvironmentVariableA("BB_COOP", "1");
        CHECK(load_alone(argv[1], "coop", writes));
        CHECK(!strstr(g_log, "online play") && strstr(g_log, "coop: online writes true true true true"));
        CHECK(g_flags[6] == 1 && !strcmp(g_weapon_attack, "7") && g_echoes == 1);
        SetEnvironmentVariableA("BB_COOP", NULL);
        SetEnvironmentVariableA("BB_ONLINE", NULL);
    }

    /* Alone, each in a fresh copy of the DLL: loops that catch the budget error. */
    g_api = api;
    CHECK(stopped_alone(argv[1], "pcall_loop", "bb.on_frame(function() while true do pcall(function() while true do end end) end end)\n"));
    CHECK(stopped_alone(argv[1], "xpcall_nested",
                        "bb.on_frame(function()\n"
                        "  while true do xpcall(function() pcall(function() while true do end end) end, function(e) return e end) end\n"
                        "end)\n"));

    printf("lua host: %d checks passed\n", checks);
    return 0;
}

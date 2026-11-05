/* SPDX-License-Identifier: GPL-3.0-or-later
 * bb_lua: Lua 5.4 scripts as plugins, so a mod can change the game without a compiler. A native
 * plugin (bbhost's API) like any other; each .lua file is a script with its own Lua state.
 *
 * Where scripts come from, loaded in this order during bb_plugin_image (so they may register what
 * must be registered then, bb.flag.on):
 *   - <user>\plugins\lua\*.lua (this plugin's plugin_dir), named by file: "hello";
 *   - <mods>\<mod>\scripts\*.lua for each mod not disabled in mods.json, named "<mod>/<file>", so two
 *     mods may each have a main.lua. A script's plugins.ini section is its name: [mymod/main].
 *
 * What a script may do: the `bb` table (docs/LUA.md) and a reduced standard library: base, table,
 * string, math, utf8, coroutine, and os.clock/os.time/os.date. Nothing that reads or writes files,
 * runs programs, or loads code from text or bytecode: no io, os.execute, load, loadfile, dofile,
 * require, string.dump, debug.
 *
 * What keeps one script from hurting the game or the others:
 *   - every call into a script is protected (lua_pcall); an error stops that script only, with a
 *     log line and a banner;
 *   - each call has an instruction budget (BUDGET), so an endless loop stops the script instead of
 *     freezing the game;
 *   - each script's memory is capped (MEMORY_CAP); a script over it stops. */
#include "bbhost_plugin.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

enum { MAX_SCRIPTS = 64, NAME_LEN = 64 };
static lua_Hook count_hook_fn(void);
static const long long BUDGET = 20000000;            /* VM instructions per call into a script */
static const size_t MEMORY_CAP = 64u * 1024 * 1024;  /* bytes per script */

enum { HOOK_EVERY = 10000 };

typedef struct {
    char name[NAME_LEN];  /* the file name without .lua: its log prefix and plugins.ini section */
    lua_State *L;
    size_t memory;
    long long left;       /* the current call's budget */
    int spent;            /* the budget ran out in this call: every instruction raises until it ends */
    int on;
} Script;

static const BbHostApi *g_api;
static Script g_scripts[MAX_SCRIPTS];
static int g_count;

/* --- limits ------------------------------------------------------------------------------------ */

static void *capped_alloc(void *ud, void *ptr, size_t old, size_t size) {
    Script *s = ud;
    if (!ptr) old = 0;
    if (size == 0) {
        free(ptr);
        s->memory -= old;
        return NULL;
    }
    if (size > old && s->memory - old + size > MEMORY_CAP) return NULL;  /* Lua raises "not enough memory" */
    void *p = realloc(ptr, size);
    if (p) s->memory = s->memory - old + size;
    return p;
}

static Script *script_of(lua_State *L) {
    void *ud = NULL;
    lua_getallocf(L, &ud);
    return ud;
}

/* Once the budget is spent the hook fires on every instruction, here and in the script's main
 * thread, and raises each time: a pcall (or a coroutine) may catch one error, but the next
 * instruction outside it raises again, so no loop survives. tool-711f289-F1. */
static void spend(lua_State *L, Script *s, long long amount) {
    s->left -= amount;
    if (s->left > 0) return;
    if (!s->spent) {
        s->spent = 1;
        lua_sethook(s->L, count_hook_fn(), LUA_MASKCOUNT, 1);
        if (L != s->L) lua_sethook(L, count_hook_fn(), LUA_MASKCOUNT, 1);
    }
    luaL_error(L, "took too long (over %I instructions in one call)", (lua_Integer)BUDGET);
}

static void count_hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    Script *s = script_of(L);
    spend(L, s, s->spent ? 1 : HOOK_EVERY);
}

static lua_Hook count_hook_fn(void) { return count_hook; }

/* Lua's pattern matcher (the bb_lua change in lstrlib.c): its steps count against the budget too. */
void lua_pattern_tick(lua_State *L) { spend(L, script_of(L), 4096); }

/* --- the bb table --------------------------------------------------------------------------------- */

/* All arguments as text, joined by spaces (tostring on each), into one log line. */
static int l_log(lua_State *L) {
    const int n = lua_gettop(L);  /* before luaL_buffinit, which pushes a slot of its own */
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) luaL_addchar(&b, ' ');
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    g_api->log("%s: %s", script_of(L)->name, lua_tostring(L, -1));
    return 0;
}

/* bb.on_frame(fn): fn() once a frame, on the game's main thread. */
static int l_on_frame(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_getfield(L, LUA_REGISTRYINDEX, "bb_frames");
    lua_pushvalue(L, 1);
    lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
    return 0;
}

/* bb.config(key): this script's option from plugins.ini ([<script name>] key = value), as text,
 * or nil when not set. */
static int l_config(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    char full[160];
    snprintf(full, sizeof full, "%s.%s", script_of(L)->name, key);
    const char *v = g_api->config(full);
    if (*v) lua_pushstring(L, v);
    else lua_pushnil(L);
    return 1;
}

/* --- the game: flags, params, the player, effects, characters, text --------------------------------- */

/* bb.flag.get(id) -> true/false, or nil when its block doesn't exist (yet). */
static int l_flag_get(lua_State *L) {
    int v = 0;
    if (g_api->event_flag_get((uint32_t)luaL_checkinteger(L, 1), &v)) lua_pushnil(L);
    else lua_pushboolean(L, v);
    return 1;
}

/* bb.flag.set(id, on) -> true when written. */
static int l_flag_set(lua_State *L) {
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    lua_pushboolean(L, !g_api->event_flag_set((uint32_t)luaL_checkinteger(L, 1), lua_toboolean(L, 2)));
    return 1;
}

static int g_flags_watched;  /* the host's on_event_flag registered (once, for all scripts) */
static void on_flag(uint32_t id, int value, void *user);
static void on_world(uint32_t block, void *user);
static int g_loads_watched;

/* Appends fn to one of the script's callback lists in the registry. */
static void add_callback(lua_State *L, const char *list, int fn) {
    lua_getfield(L, LUA_REGISTRYINDEX, list);
    lua_pushvalue(L, fn);
    lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
    lua_pop(L, 1);
}

/* bb.flag.on(fn): fn(id, on) for each flag change, at the start of a frame. Register while the
 * script loads (the host's watcher is a hook placed then). */
static int l_flag_on(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (!g_flags_watched) {
        if (g_api->on_event_flag(on_flag, NULL)) return luaL_error(L, "flag changes can't be watched (register while loading)");
        g_flags_watched = 1;
    }
    add_callback(L, "bb_flags", 1);
    return 0;
}

/* bb.on_world_load(fn): fn(block) when the player is placed in a map (a load, a warp). */
static int l_on_world_load(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (!g_loads_watched) {
        if (g_api->on_world_load(on_world, NULL)) return luaL_error(L, "world loads can't be watched");
        g_loads_watched = 1;
    }
    add_callback(L, "bb_loads", 1);
    return 0;
}

/* bb.param.get(table, id, field) -> text, or nil. */
static int l_param_get(lua_State *L) {
    char out[256];
    if (g_api->param_get(luaL_checkstring(L, 1), (uint32_t)luaL_checkinteger(L, 2), luaL_checkstring(L, 3), out, sizeof out))
        lua_pushnil(L);
    else
        lua_pushstring(L, out);
    return 1;
}

/* bb.param.set(table, id, field, value) -> true when written (value: a number or text). */
static int l_param_set(lua_State *L) {
    const char *value = luaL_tolstring(L, 4, NULL);
    lua_pushboolean(L, !g_api->param_set(luaL_checkstring(L, 1), (uint32_t)luaL_checkinteger(L, 2), luaL_checkstring(L, 3), value));
    return 1;
}

/* bb.param.ids(table) -> {ids...}, or nil when the table isn't loaded. */
static int l_param_ids(lua_State *L) {
    const char *table = luaL_checkstring(L, 1);
    size_t count = 0;
    if (g_api->param_ids(table, NULL, 0, &count)) {
        lua_pushnil(L);
        return 1;
    }
    uint32_t *ids = count ? malloc(count * sizeof *ids) : NULL;
    if (count && (!ids || g_api->param_ids(table, ids, count, &count))) {
        free(ids);
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, (int)count, 0);
    for (size_t i = 0; i < count; ++i) {
        lua_pushinteger(L, ids[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    free(ids);
    return 1;
}

/* bb.player.stat(name) -> number or nil; bb.player.stat(name, value) -> true when written. */
static int l_player_stat(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    if (lua_gettop(L) >= 2) {
        lua_pushboolean(L, !g_api->player_stat_set(name, (int64_t)luaL_checkinteger(L, 2)));
        return 1;
    }
    int64_t v = 0;
    if (g_api->player_stat_get(name, &v)) lua_pushnil(L);
    else lua_pushinteger(L, (lua_Integer)v);
    return 1;
}

/* bb.player.position() -> x, y, z, yaw, or nil with no player. */
static int l_player_position(lua_State *L) {
    float p[3], yaw = 0;
    if (g_api->player_position(p, &yaw)) {
        lua_pushnil(L);
        return 1;
    }
    for (int i = 0; i < 3; ++i) lua_pushnumber(L, p[i]);
    lua_pushnumber(L, yaw);
    return 4;
}

/* bb.player.block() -> the map block (0x18010000 for m24_01_00_00), or nil. */
static int l_player_block(lua_State *L) {
    uint32_t b = 0;
    if (g_api->player_block(&b)) lua_pushnil(L);
    else lua_pushinteger(L, b);
    return 1;
}

/* A character argument: nil (or absent) for the main player, else a bb.chr.list() entry's .ins. */
static void *chr_arg(lua_State *L, int at) {
    if (lua_isnoneornil(L, at)) return NULL;
    luaL_checktype(L, at, LUA_TLIGHTUSERDATA);
    return lua_touserdata(L, at);
}

/* bb.effect.apply(id[, chr]) -> true when applied. bb.effect.has(id[, chr]) -> true/false. */
static int l_effect_apply(lua_State *L) {
    lua_pushboolean(L, !g_api->sp_effect_apply(chr_arg(L, 2), (int32_t)luaL_checkinteger(L, 1)));
    return 1;
}
static int l_effect_has(lua_State *L) {
    lua_pushboolean(L, g_api->sp_effect_has(chr_arg(L, 2), (int32_t)luaL_checkinteger(L, 1)) != 0);
    return 1;
}

/* bb.chr.list() -> {{ins=, handle=, npc=, model=, hp=, max_hp=, team=, type=, player=, x=, y=, z=}, ...}
 * or nil before the world exists. The main player first. Use within the frame. */
static int l_chr_list(lua_State *L) {
    size_t count = 0;
    if (g_api->chr_list(NULL, 0, &count)) {
        lua_pushnil(L);
        return 1;
    }
    BbChr *list = count ? calloc(count, sizeof *list) : NULL;
    if (count && (!list || g_api->chr_list(list, count, &count))) {
        free(list);
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, (int)count, 0);
    for (size_t i = 0; i < count; ++i) {
        const BbChr *c = &list[i];
        lua_createtable(L, 0, 12);
        lua_pushlightuserdata(L, c->ins);
        lua_setfield(L, -2, "ins");
        lua_pushinteger(L, c->handle);
        lua_setfield(L, -2, "handle");
        lua_pushinteger(L, c->npc_param);
        lua_setfield(L, -2, "npc");
        lua_pushinteger(L, c->model);
        lua_setfield(L, -2, "model");
        lua_pushinteger(L, c->hp);
        lua_setfield(L, -2, "hp");
        lua_pushinteger(L, c->max_hp);
        lua_setfield(L, -2, "max_hp");
        lua_pushinteger(L, c->team_type);
        lua_setfield(L, -2, "team");
        lua_pushinteger(L, c->chr_type);
        lua_setfield(L, -2, "type");
        lua_pushboolean(L, c->is_player);
        lua_setfield(L, -2, "player");
        lua_pushnumber(L, c->pos[0]);
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, c->pos[1]);
        lua_setfield(L, -2, "y");
        lua_pushnumber(L, c->pos[2]);
        lua_setfield(L, -2, "z");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    free(list);
    return 1;
}

/* bb.message(text[, seconds]) -> true when shown: a banner over the game (default 3 s). */
static int l_message(lua_State *L) {
    lua_pushboolean(L, !g_api->show_message(luaL_checkstring(L, 1), (float)luaL_optnumber(L, 2, 3.0)));
    return 1;
}

/* bb.text(text, seconds, x, y[, scale[, align]]) -> true when shown. align: "left" (default),
 * "centre"/"center", "right", each optionally with " bottom" (the box rises from the point). */
static int l_text(lua_State *L) {
    const char *text = luaL_checkstring(L, 1);
    const float seconds = (float)luaL_checknumber(L, 2), x = (float)luaL_checknumber(L, 3), y = (float)luaL_checknumber(L, 4);
    const float scale = (float)luaL_optnumber(L, 5, 1.0);
    const char *align = luaL_optstring(L, 6, "left");
    uint32_t a = BB_TEXT_LEFT;
    if (strstr(align, "cent")) a = BB_TEXT_CENTRE;
    else if (strstr(align, "right")) a = BB_TEXT_RIGHT;
    if (strstr(align, "bottom")) a |= BB_TEXT_BOTTOM;
    lua_pushboolean(L, !g_api->show_text(text, seconds, x, y, scale, a));
    return 1;
}

static const luaL_Reg FLAG[] = {{"get", l_flag_get}, {"set", l_flag_set}, {"on", l_flag_on}, {NULL, NULL}};
static const luaL_Reg PARAM[] = {{"get", l_param_get}, {"set", l_param_set}, {"ids", l_param_ids}, {NULL, NULL}};
static const luaL_Reg PLAYER[] = {{"stat", l_player_stat}, {"position", l_player_position}, {"block", l_player_block}, {NULL, NULL}};
static const luaL_Reg EFFECT[] = {{"apply", l_effect_apply}, {"has", l_effect_has}, {NULL, NULL}};
static const luaL_Reg CHR[] = {{"list", l_chr_list}, {NULL, NULL}};

static const luaL_Reg BB[] = {
    {"log", l_log},
    {"on_frame", l_on_frame},
    {"config", l_config},
    {"on_world_load", l_on_world_load},
    {"message", l_message},
    {"text", l_text},
    {NULL, NULL},
};

/* --- states ------------------------------------------------------------------------------------- */

static void open_sandbox(lua_State *L) {
    static const luaL_Reg libs[] = {
        {LUA_GNAME, luaopen_base},       {LUA_TABLIBNAME, luaopen_table}, {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8}, {LUA_COLIBNAME, luaopen_coroutine},
        {NULL, NULL},
    };
    for (const luaL_Reg *lib = libs; lib->func; ++lib) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }
    /* No code from text, files or bytecode. */
    static const char *const removed[] = {"dofile", "loadfile", "load", NULL};
    for (const char *const *r = removed; *r; ++r) {
        lua_pushnil(L);
        lua_setglobal(L, *r);
    }
    lua_getglobal(L, "string");
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pop(L, 1);
    /* os: the clock and dates only. */
    luaL_requiref(L, "os_full", luaopen_os, 0);
    lua_newtable(L);
    static const char *const kept[] = {"clock", "time", "date", NULL};
    for (const char *const *k = kept; *k; ++k) {
        lua_getfield(L, -2, *k);
        lua_setfield(L, -2, *k);
    }
    lua_setglobal(L, "os");
    lua_pop(L, 1);
    /* bb, and print as bb.log. */
    luaL_newlib(L, BB);
    static const struct {
        const char *name;
        const luaL_Reg *fns;
    } groups[] = {{"flag", FLAG}, {"param", PARAM}, {"player", PLAYER}, {"effect", EFFECT}, {"chr", CHR}};
    /* Argument errors name the function from the loaded-modules table: listing bb and its parts
     * there makes them say "bb.effect.has" (tool-9bb9566-N1). Scripts can't reach the table. */
    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    for (size_t i = 0; i < sizeof groups / sizeof *groups; ++i) {
        lua_newtable(L);
        luaL_setfuncs(L, groups[i].fns, 0);
        lua_pushvalue(L, -1);
        char loaded_name[32];
        snprintf(loaded_name, sizeof loaded_name, "bb.%s", groups[i].name);
        lua_setfield(L, -3, loaded_name);
        lua_setfield(L, -3, groups[i].name);
    }
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, "bb");
    lua_pop(L, 1);
    lua_setglobal(L, "bb");
    lua_pushcfunction(L, l_log);
    lua_setglobal(L, "print");
    static const char *const lists[] = {"bb_frames", "bb_flags", "bb_loads", NULL};
    for (const char *const *l = lists; *l; ++l) {
        lua_newtable(L);
        lua_setfield(L, LUA_REGISTRYINDEX, *l);
    }
}

/* Stops a script for good: the reason to the log and on screen. */
static void stop(Script *s, const char *why) {
    s->on = 0;
    g_api->log("%s: stopped: %s", s->name, why);
    char banner[160];
    snprintf(banner, sizeof banner, "Lua script %s stopped (see the log)", s->name);
    g_api->show_message(banner, 5.0f);
}

/* Calls the function on top of the stack with no arguments, within the budget. 0 on success. */
static int call(Script *s, int nargs) {
    s->left = BUDGET;
    if (s->spent) {  /* defensive: a spent call always ends in an error, which stops the script */
        s->spent = 0;
        lua_sethook(s->L, count_hook, LUA_MASKCOUNT, HOOK_EVERY);
    }
    if (lua_pcall(s->L, nargs, 0, 0) == LUA_OK) return 0;
    const char *err = lua_tostring(s->L, -1);
    char why[256];
    snprintf(why, sizeof why, "%s", err ? err : "an error without a message");
    lua_pop(s->L, 1);
    stop(s, why);
    return 1;
}

static void load_script(const wchar_t *path, const wchar_t *file, const char *prefix) {
    if (g_count == MAX_SCRIPTS) {
        g_api->log("more than %d scripts: the rest are left out", MAX_SCRIPTS);
        return;
    }
    Script *s = &g_scripts[g_count];
    memset(s, 0, sizeof *s);
    char stem[NAME_LEN];
    WideCharToMultiByte(CP_UTF8, 0, file, -1, stem, NAME_LEN, NULL, NULL);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    snprintf(s->name, NAME_LEN, "%s%s", prefix, stem);  /* "mod/script" for a mod's scripts */
    char utf8_path[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8_path, sizeof utf8_path, NULL, NULL);
    /* The source, read here (no io for scripts). */
    FILE *f = _wfopen(path, L"rb");
    if (!f) {
        g_api->log("%s: cannot be read", s->name);
        return;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = size > 0 && size < 4 * 1024 * 1024 ? malloc((size_t)size) : NULL;
    const size_t got = text ? fread(text, 1, (size_t)size, f) : 0;
    fclose(f);
    if (!text || got != (size_t)size) {
        free(text);
        g_api->log("%s: empty, too big (over 4 MiB) or unreadable", s->name);
        return;
    }
    s->L = lua_newstate(capped_alloc, s);
    if (!s->L) {
        free(text);
        return;
    }
    s->on = 1;
    ++g_count;
    open_sandbox(s->L);
    lua_sethook(s->L, count_hook, LUA_MASKCOUNT, HOOK_EVERY);
    char chunk[NAME_LEN + 2];
    snprintf(chunk, sizeof chunk, "@%s", s->name);
    /* Text only ("t"): bytecode could break the VM's own checks. */
    const int loaded = luaL_loadbufferx(s->L, text, (size_t)size, chunk, "t");
    free(text);
    if (loaded != LUA_OK) {
        stop(s, lua_tostring(s->L, -1));
        return;
    }
    if (!call(s, 0)) g_api->log("%s: loaded (%s)", s->name, utf8_path);
}

static void load_folder(const char *folder, const char *prefix) {
    if (!folder || !*folder) return;
    wchar_t dir[MAX_PATH], pattern[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, folder, -1, dir, MAX_PATH)) return;
    size_t len = wcslen(dir);
    while (len && (dir[len - 1] == L'\\' || dir[len - 1] == L'/')) dir[--len] = 0;  /* plugin_dir ends in one */
    _snwprintf(pattern, MAX_PATH, L"%ls\\*.lua", dir);
    WIN32_FIND_DATAW found;
    HANDLE h = FindFirstFileW(pattern, &found);
    if (h == INVALID_HANDLE_VALUE) return;
    wchar_t names[MAX_SCRIPTS][MAX_PATH];
    int n = 0;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n < MAX_SCRIPTS) wcscpy(names[n++], found.cFileName);
    } while (FindNextFileW(h, &found));
    FindClose(h);
    for (int i = 1; i < n; ++i)  /* name order */
        for (int j = i; j > 0 && _wcsicmp(names[j - 1], names[j]) > 0; --j) {
            wchar_t t[MAX_PATH];
            wcscpy(t, names[j]);
            wcscpy(names[j], names[j - 1]);
            wcscpy(names[j - 1], t);
        }
    for (int i = 0; i < n; ++i) {
        wchar_t path[MAX_PATH];
        _snwprintf(path, MAX_PATH, L"%ls\\%ls", dir, names[i]);
        load_script(path, names[i], prefix);
    }
}

/* --- scripts that come with mods ------------------------------------------------------------------- */

/* The player's mod settings: the launcher's mods.json, the file scripts/mods.py reads
 * (BB_MODS_CONFIG; else beside the mods folder). {"disabled": ["name", ...], "order": [...]}. */
enum { MAX_MODS = 256 };
typedef struct {
    char disabled[MAX_MODS][NAME_LEN];
    int ndisabled;
    char order[MAX_MODS][NAME_LEN];
    int norder;
} ModSettings;

static void put_utf8(char *out, size_t *w, size_t cap, unsigned code) {
    char b[4];
    size_t n = 0;
    if (code < 0x80) b[n++] = (char)code;
    else if (code < 0x800) b[n++] = (char)(0xc0 | code >> 6), b[n++] = (char)(0x80 | (code & 0x3f));
    else b[n++] = (char)(0xe0 | code >> 12), b[n++] = (char)(0x80 | ((code >> 6) & 0x3f)), b[n++] = (char)(0x80 | (code & 0x3f));
    for (size_t i = 0; i < n && *w + 1 < cap; ++i) out[(*w)++] = b[i];
}

/* A JSON string at *p (on its opening quote) into out, UTF-8; *p ends after the closing quote.
 * 0 on a malformed string. Surrogate pairs come out as two 3-byte sequences (names are BMP). */
static int json_string(const char **p, char *out, size_t cap) {
    const char *s = *p;
    size_t w = 0;
    if (*s++ != '"') return 0;
    while (*s && *s != '"') {
        unsigned c = (unsigned char)*s++;
        if (c == '\\') {
            const char e = *s++;
            if (e == 'u') {
                unsigned code = 0;
                for (int i = 0; i < 4; ++i, ++s) {
                    const char h = *s;
                    if (!h) return 0;
                    code = code * 16 + (unsigned)(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
                }
                put_utf8(out, &w, cap, code);
                continue;
            }
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : (unsigned char)e;
            if (!e) return 0;
        }
        if (w + 1 < cap) out[w++] = (char)c;
    }
    if (*s != '"') return 0;
    out[w] = 0;
    *p = s + 1;
    return 1;
}

static const char *skip_space(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}

/* Skips one JSON value of any kind; NULL when malformed. */
static const char *skip_value(const char *p) {
    p = skip_space(p);
    if (*p == '"') {
        /* a string of any length: scan, honouring escapes */
        ++p;
        while (*p && *p != '"') p += (*p == '\\' && p[1]) ? 2 : 1;
        return *p ? p + 1 : NULL;
    }
    if (*p == '[' || *p == '{') {
        const char close = *p == '[' ? ']' : '}';
        p = skip_space(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            if (close == '}') {
                if (!(p = skip_value(p))) return NULL;  /* the key */
                p = skip_space(p);
                if (*p++ != ':') return NULL;
            }
            if (!(p = skip_value(p))) return NULL;
            p = skip_space(p);
            if (*p == ',') { p = skip_space(p + 1); continue; }
            return *p == close ? p + 1 : NULL;
        }
    }
    while (*p && *p != ',' && *p != ']' && *p != '}' && *p != ' ' && *p != '\r' && *p != '\n') ++p;  /* number, true... */
    return p;
}

/* The top-level object's string arrays "disabled" and "order". Unknown keys and other shapes are
 * skipped; a malformed file leaves what was read so far. */
static void parse_mod_settings(const char *text, ModSettings *m) {
    const char *p = skip_space(text);
    if (*p++ != '{') return;
    for (;;) {
        p = skip_space(p);
        char key[32];
        if (*p != '"' || !json_string(&p, key, sizeof key)) return;
        p = skip_space(p);
        if (*p++ != ':') return;
        p = skip_space(p);
        const int disabled = !strcmp(key, "disabled"), order = !strcmp(key, "order");
        if ((disabled || order) && *p == '[') {
            p = skip_space(p + 1);
            while (*p == '"') {
                char name[NAME_LEN * 2];
                if (!json_string(&p, name, sizeof name)) return;
                int *n = disabled ? &m->ndisabled : &m->norder;
                char (*list)[NAME_LEN] = disabled ? m->disabled : m->order;
                if (*n < MAX_MODS && strlen(name) < NAME_LEN) strcpy(list[(*n)++], name);
                p = skip_space(p);
                if (*p == ',') p = skip_space(p + 1);
            }
            if (*p++ != ']') return;
        } else if (!(p = skip_value(p))) {
            return;
        }
        p = skip_space(p);
        if (*p == ',') { ++p; continue; }
        return;
    }
}

static void load_mod_settings(const char *mods_dir, ModSettings *m) {
    memset(m, 0, sizeof *m);
    wchar_t path[MAX_PATH];
    const wchar_t *env = _wgetenv(L"BB_MODS_CONFIG");
    if (env && *env) {
        wcsncpy(path, env, MAX_PATH - 1);
        path[MAX_PATH - 1] = 0;
    } else {
        wchar_t dir[MAX_PATH];
        if (!MultiByteToWideChar(CP_UTF8, 0, mods_dir, -1, dir, MAX_PATH)) return;
        _snwprintf(path, MAX_PATH, L"%ls\\..\\mods.json", dir);
    }
    FILE *f = _wfopen(path, L"rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = size > 0 && size < 16 * 1024 * 1024 ? malloc((size_t)size + 1) : NULL;
    if (text && fread(text, 1, (size_t)size, f) == (size_t)size) {
        text[size] = 0;
        parse_mod_settings(text, m);
    }
    free(text);
    fclose(f);
}

static int listed(char (*list)[NAME_LEN], int n, const char *name) {
    for (int i = 0; i < n; ++i)
        if (!strcmp(list[i], name)) return i;
    return -1;
}

/* <mods>\<mod>\scripts\*.lua for every mod folder not disabled, in the player's order (mods.json),
 * then the rest in folder-name order, as scripts/mods.py orders the mods' files. */
static void load_mods(const char *mods_dir) {
    if (!mods_dir || !*mods_dir) return;
    static ModSettings settings;
    load_mod_settings(mods_dir, &settings);
    wchar_t dir[MAX_PATH], pattern[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, mods_dir, -1, dir, MAX_PATH)) return;
    _snwprintf(pattern, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW found;
    HANDLE h = FindFirstFileW(pattern, &found);
    if (h == INVALID_HANDLE_VALUE) return;
    static wchar_t mods[MAX_MODS][MAX_PATH];
    static char names[MAX_MODS][NAME_LEN];
    static int rank[MAX_MODS];
    int n = 0;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && found.cFileName[0] != L'.' && n < MAX_MODS) {
            wcscpy(mods[n], found.cFileName);
            WideCharToMultiByte(CP_UTF8, 0, found.cFileName, -1, names[n], NAME_LEN, NULL, NULL);
            const int at = listed(settings.order, settings.norder, names[n]);
            rank[n] = at >= 0 ? at : MAX_MODS;  /* unlisted after the listed ones */
            ++n;
        }
    } while (FindNextFileW(h, &found));
    FindClose(h);
    for (int i = 1; i < n; ++i)  /* by rank, then by name */
        for (int j = i; j > 0 && (rank[j - 1] > rank[j] || (rank[j - 1] == rank[j] && _wcsicmp(mods[j - 1], mods[j]) > 0)); --j) {
            wchar_t tw[MAX_PATH];
            char tn[NAME_LEN];
            const int tr = rank[j];
            wcscpy(tw, mods[j]), wcscpy(mods[j], mods[j - 1]), wcscpy(mods[j - 1], tw);
            strcpy(tn, names[j]), strcpy(names[j], names[j - 1]), strcpy(names[j - 1], tn);
            rank[j] = rank[j - 1], rank[j - 1] = tr;
        }
    for (int i = 0; i < n; ++i) {
        wchar_t scripts[MAX_PATH];
        _snwprintf(scripts, MAX_PATH, L"%ls\\%ls\\scripts", dir, mods[i]);
        const DWORD attributes = GetFileAttributesW(scripts);
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (listed(settings.disabled, settings.ndisabled, names[i]) >= 0) {
            g_api->log("mod %s is disabled (mods.json): its scripts are not loaded", names[i]);
            continue;
        }
        char folder[MAX_PATH * 3], prefix[NAME_LEN + 2];
        WideCharToMultiByte(CP_UTF8, 0, scripts, -1, folder, sizeof folder, NULL, NULL);
        snprintf(prefix, sizeof prefix, "%s/", names[i]);
        load_folder(folder, prefix);
    }
}

/* --- the host's callbacks ----------------------------------------------------------------------- */

/* Each running script's callbacks in one of its lists, each called with what push() pushes. */
static void dispatch(const char *list, void (*push)(lua_State *L, const void *what), const void *what, int nargs) {
    for (int i = 0; i < g_count; ++i) {
        Script *s = &g_scripts[i];
        if (!s->on) continue;
        lua_getfield(s->L, LUA_REGISTRYINDEX, list);
        const lua_Integer n = (lua_Integer)lua_rawlen(s->L, -1);
        for (lua_Integer k = 1; k <= n && s->on; ++k) {
            lua_rawgeti(s->L, -1, k);
            if (push) push(s->L, what);
            call(s, nargs);
        }
        lua_settop(s->L, 0);
    }
}

typedef struct {
    uint32_t id;
    int value;
} FlagChange;

static void push_flag(lua_State *L, const void *what) {
    const FlagChange *c = what;
    lua_pushinteger(L, c->id);
    lua_pushboolean(L, c->value);
}

static void push_block(lua_State *L, const void *what) { lua_pushinteger(L, *(const uint32_t *)what); }

static void on_flag(uint32_t id, int value, void *user) {
    (void)user;
    const FlagChange c = {id, value};
    dispatch("bb_flags", push_flag, &c, 2);
}

static void on_world(uint32_t block, void *user) {
    (void)user;
    dispatch("bb_loads", push_block, &block, 1);
}

static void on_frame(void *user) {
    (void)user;
    dispatch("bb_frames", NULL, NULL, 0);
}

BB_PLUGIN_EXPORT const BbPluginInfo bb_plugin_info = {
    BB_PLUGIN_API_VERSION, 0, "lua", "Lua scripts", "0.1.0", "moding_tool",
    "Runs Lua scripts from plugins\\lua: mods without a compiler.",
};

BB_PLUGIN_EXPORT int bb_plugin_image(const BbHostApi *api) {
    g_api = api;
    if (api->version < 16 || api->size < sizeof(BbHostApi)) {
        api->log("this host's API is older than 16: not starting");
        return 1;
    }
    load_folder(api->plugin_dir("lua"), "");
    load_mods(api->mods_dir ? api->mods_dir() : NULL);
    int on = 0;
    for (int i = 0; i < g_count; ++i) on += g_scripts[i].on;
    api->log("%d script(s) running, %d stopped", on, g_count - on);
    if (!g_count) return 1;  /* nothing to run: unload */
    return api->on_frame(on_frame, NULL);
}

/* SPDX-License-Identifier: GPL-3.0-or-later
 * bb_lua: Lua 5.4 scripts as plugins, so a mod can change the game without a compiler. A native
 * plugin (bbhost's API) like any other; each .lua file is a script with its own Lua state.
 *
 * Where scripts come from: <user>\plugins\lua\*.lua (this plugin's plugin_dir), loaded in name order
 * during bb_plugin_image, so they may register what must be registered then (bb.flag.on).
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

static const luaL_Reg BB[] = {
    {"log", l_log},
    {"on_frame", l_on_frame},
    {"config", l_config},
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
    lua_setglobal(L, "bb");
    lua_pushcfunction(L, l_log);
    lua_setglobal(L, "print");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "bb_frames");
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

static void load_script(const wchar_t *path, const wchar_t *file) {
    if (g_count == MAX_SCRIPTS) {
        g_api->log("more than %d scripts: the rest are left out", MAX_SCRIPTS);
        return;
    }
    Script *s = &g_scripts[g_count];
    memset(s, 0, sizeof *s);
    WideCharToMultiByte(CP_UTF8, 0, file, -1, s->name, NAME_LEN, NULL, NULL);
    char *dot = strrchr(s->name, '.');
    if (dot) *dot = 0;
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

static void load_folder(const char *folder) {
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
        load_script(path, names[i]);
    }
}

/* --- the host's callbacks ----------------------------------------------------------------------- */

static void on_frame(void *user) {
    (void)user;
    for (int i = 0; i < g_count; ++i) {
        Script *s = &g_scripts[i];
        if (!s->on) continue;
        lua_getfield(s->L, LUA_REGISTRYINDEX, "bb_frames");
        const lua_Integer n = (lua_Integer)lua_rawlen(s->L, -1);
        for (lua_Integer k = 1; k <= n && s->on; ++k) {
            lua_rawgeti(s->L, -1, k);
            call(s, 0);
        }
        lua_settop(s->L, 0);
    }
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
    load_folder(api->plugin_dir("lua"));
    int on = 0;
    for (int i = 0; i < g_count; ++i) on += g_scripts[i].on;
    api->log("%d script(s) running, %d stopped", on, g_count - on);
    if (!g_count) return 1;  /* nothing to run: unload */
    return api->on_frame(on_frame, NULL);
}

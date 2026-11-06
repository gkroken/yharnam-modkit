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

enum { MAX_SCRIPTS = 64 };
static lua_Hook count_hook_fn(void);
static const long long BUDGET = 20000000;            /* VM instructions per call into a script */
static const size_t MEMORY_CAP = 64u * 1024 * 1024;  /* bytes per script */

enum { HOOK_EVERY = 10000 };

typedef struct {
    char *name;  /* the file name without .lua, whole (heap): its log prefix and plugins.ini section */
    lua_State *L;
    size_t memory;
    long long left;       /* the current call's budget */
    int spent;            /* the budget ran out in this call: every instruction raises until it ends */
    int on;
    int told_online;      /* its first refused change in online play was logged */
    int told_warp;        /* its first warp refused for want of a grant was logged */
    uint32_t take_keys[16];  /* bb.input.take: its keys (scancode bits), buttons, wheel, motion */
    uint32_t take_buttons;
    int take_wheel, take_motion;
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
    const char *name = script_of(L)->name;
    char *full = malloc(strlen(name) + strlen(key) + 2);  /* whole: a cut key would read another's */
    if (!full) return luaL_error(L, "out of memory");
    sprintf(full, "%s.%s", name, key);
    const char *v = g_api->config(full);
    free(full);
    if (*v) lua_pushstring(L, v);
    else lua_pushnil(L);
    return 1;
}

/* --- the game: flags, params, the player, effects, characters, text --------------------------------- */

/* Online play (tool-9614890-F1): the host leaves out native plugins that change the game, the way
 * cheats are turned off. Scripts can't be told apart that way, so here every call that changes the
 * game does nothing and returns false; reading, watching and text still work. The host's own test:
 * BB_ONLINE=1 without BB_COOP=1. */
static int g_online;

static int env_is(const char *name, const char *value) {
    char got[16];
    const DWORD n = GetEnvironmentVariableA(name, got, sizeof got);
    return n && n < sizeof got && !strcmp(got, value);
}

/* 1 (and the call should return false) when this is online play. Logged once per script. */
static int refused_online(lua_State *L, const char *call) {
    if (!g_online) return 0;
    Script *s = script_of(L);
    if (!s->told_online) {
        s->told_online = 1;
        g_api->log("%s: online play: %s and every other call that changes the game do nothing", s->name, call);
    }
    lua_pushboolean(L, 0);
    return 1;
}

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
    const uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    if (refused_online(L, "bb.flag.set")) return 1;
    lua_pushboolean(L, !g_api->event_flag_set(id, lua_toboolean(L, 2)));
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
    luaL_checkstring(L, 1);
    luaL_checkinteger(L, 2);
    luaL_checkstring(L, 3);
    if (refused_online(L, "bb.param.set")) return 1;
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
        const int64_t value = (int64_t)luaL_checkinteger(L, 2);
        if (refused_online(L, "bb.player.stat (setting)")) return 1;
        lua_pushboolean(L, !g_api->player_stat_set(name, value));
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
    void *chr = chr_arg(L, 2);
    const int32_t id = (int32_t)luaL_checkinteger(L, 1);
    if (refused_online(L, "bb.effect.apply")) return 1;
    lua_pushboolean(L, !g_api->sp_effect_apply(chr, id));
    return 1;
}
static int l_effect_has(lua_State *L) {
    lua_pushboolean(L, g_api->sp_effect_has(chr_arg(L, 2), (int32_t)luaL_checkinteger(L, 1)) != 0);
    return 1;
}

/* Warps need three things: not online play; the script's own grant in plugins.ini
 * ([warps] <script name> = 1, so a downloaded mod's script can't move the player because some
 * other script was allowed to); and the host's grant to the Lua host ([warps] lua = 1), which
 * the host checks itself. 1 (and false pushed) when refused here. */
static int refused_warp(lua_State *L, const char *call) {
    if (refused_online(L, call)) return 1;
    Script *s = script_of(L);
    char *key = malloc(strlen(s->name) + 8);
    if (!key) return luaL_error(L, "out of memory");
    sprintf(key, "warps.%s", s->name);
    const char *v = g_api->config(key);
    free(key);
    if (!_stricmp(v, "1") || !_stricmp(v, "true") || !_stricmp(v, "on")) return 0;
    if (!s->told_warp) {
        s->told_warp = 1;
        g_api->log("%s: %s refused: this script may warp only with \"%s = 1\" under [warps] in plugins.ini "
                   "(and \"lua = 1\" there for the Lua host)", s->name, call, s->name);
    }
    lua_pushboolean(L, 0);
    return 1;
}

/* bb.warp.to(x, y, z[, yaw[, block]]) -> true when queued: the player moves there at the start
 * of the next frame, in its own block (or a resident one, as bb.player.block packs it), facing
 * yaw (radians; the current facing when left out). */
static int l_warp_to(lua_State *L) {
    float xyz[3] = {(float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3)};
    float yaw = 0.0f, here[3];
    if (lua_isnoneornil(L, 4)) {
        if (g_api->player_position(here, &yaw)) {
            lua_pushboolean(L, 0);  /* no player */
            return 1;
        }
    } else {
        yaw = (float)luaL_checknumber(L, 4);
    }
    const uint32_t block = (uint32_t)luaL_optinteger(L, 5, 0);
    if (refused_warp(L, "bb.warp.to")) return 1;
    lua_pushboolean(L, !g_api->player_warp(block, xyz, yaw));
    return 1;
}

/* bb.warp.lamp(return_point) -> true when queued: travel to a lamp, a full load, by its
 * ReturnPointParam row (the lamp's entity id + 1000: 2412951 is Central Yharnam's). */
static int l_warp_lamp(lua_State *L) {
    const lua_Integer rp = luaL_checkinteger(L, 1);
    luaL_argcheck(L, rp > 0 && rp <= INT32_MAX, 1, "a ReturnPointParam row");
    if (refused_warp(L, "bb.warp.lamp")) return 1;
    lua_pushboolean(L, !g_api->lamp_warp((int32_t)rp));
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

static int g_loading_scripts;  /* 1 while bb_plugin_image loads the scripts: the image phase */

/* One address and text from the stack into a host item: the text as a new UTF-16 string. */
static void text_item(lua_State *L, int at_index, int text_index, BbText *out) {
    const lua_Integer at = luaL_checkinteger(L, at_index);
    size_t len = 0;
    const char *text = luaL_checklstring(L, text_index, &len);
    luaL_argcheck(L, at >= 0x400000, at_index, "a Binary Ninja address of the 1.09 image");
    luaL_argcheck(L, len > 0 && len <= 4096 * 3 && strlen(text) == len, text_index, "text: 1 to 4096 characters, no NUL");
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)len, NULL, 0);
    luaL_argcheck(L, n > 0 && n <= 4096, text_index, "text: valid UTF-8, up to 4096 characters");
    /* A userdata, so Lua frees it even when a later item raises an error. */
    wchar_t *wide = lua_newuserdatauv(L, ((size_t)n + 1) * sizeof *wide, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)len, wide, n);
    wide[n] = 0;
    out->bn_addr = (uint64_t)at;
    out->text = (const uint16_t *)wide;
}

/* bb.replace_text(address, text) or bb.replace_text{[address] = text, ...} -> the references
 * moved (0: nothing names those strings). The game's UTF-16 strings at Binary Ninja addresses
 * show the texts (UTF-8) instead, wherever the code and data name them. Only while the script
 * loads (its top level): the host rewrites the code before the game runs. A table is one pass
 * over the image however many strings it holds; one call per string is a pass each
 * (tool-8604ff7-N1). The first replacement of a string stays. */
static int l_replace_text(lua_State *L) {
    enum { MAX_AT_ONCE = 4096 };
    static BbText items[MAX_AT_ONCE];
    size_t n = 0;
    if (lua_istable(L, 1)) {
        lua_settop(L, 1);
        lua_newtable(L);  /* 2: keeps each UTF-16 copy alive until the host has them */
        lua_pushnil(L);
        while (lua_next(L, 1)) {  /* key at 3, value at 4 */
            if (n == MAX_AT_ONCE) return luaL_error(L, "bb.replace_text: more than %d strings at once", MAX_AT_ONCE);
            luaL_argcheck(L, lua_isinteger(L, 3), 1, "keys are Binary Ninja addresses");
            text_item(L, 3, 4, &items[n++]);
            lua_rawseti(L, 2, (lua_Integer)n);  /* the copy into the keeper */
            lua_pop(L, 1);                       /* the value; the key stays for lua_next */
        }
    } else {
        text_item(L, 1, 2, &items[n++]);
    }
    if (!g_loading_scripts) return luaL_error(L, "bb.replace_text works only while the script loads (its top level)");
    const int moved = n ? g_api->replace_text(items, n) : 0;  /* the host keeps its own copies */
    if (moved < 0) return luaL_error(L, "the host refused bb.replace_text (not the 1.09 game, or too late)");
    lua_pushinteger(L, moved);
    return 1;
}

/* bb.loading() -> true while a loading screen is up (a load, a warp, a death). */
static int l_loading(lua_State *L) {
    lua_pushboolean(L, g_api->loading_screen() != 0);
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
static const luaL_Reg WARP[] = {{"to", l_warp_to}, {"lamp", l_warp_lamp}, {NULL, NULL}};
static const luaL_Reg CHR[] = {{"list", l_chr_list}, {NULL, NULL}};
static int l_input_down(lua_State *L);
static int l_input_pressed(lua_State *L);
static int l_input_playing(lua_State *L);
static int l_input_mouse(lua_State *L);
static int l_input_on_key(lua_State *L);
static int l_input_take(lua_State *L);
static const luaL_Reg INPUT_FNS[] = {{"down", l_input_down}, {"pressed", l_input_pressed}, {"playing", l_input_playing},
                                 {"mouse", l_input_mouse}, {"on_key", l_input_on_key}, {"take", l_input_take},
                                 {NULL, NULL}};

static const luaL_Reg BB[] = {
    {"log", l_log},
    {"on_frame", l_on_frame},
    {"config", l_config},
    {"on_world_load", l_on_world_load},
    {"message", l_message},
    {"text", l_text},
    {"loading", l_loading},
    {"replace_text", l_replace_text},
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
    } groups[] = {{"flag", FLAG}, {"param", PARAM}, {"player", PLAYER}, {"effect", EFFECT}, {"chr", CHR}, {"input", INPUT_FNS}, {"warp", WARP}};
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
    static const char *const lists[] = {"bb_frames", "bb_flags", "bb_loads", "bb_keys", NULL};
    for (const char *const *l = lists; *l; ++l) {
        lua_newtable(L);
        lua_setfield(L, LUA_REGISTRYINDEX, *l);
    }
}

/* Stops a script for good: the reason to the log and on screen. */
static int push_takes(void);

static void stop(Script *s, const char *why) {
    s->on = 0;
    /* What it took goes back to the game. */
    memset(s->take_keys, 0, sizeof s->take_keys);
    s->take_buttons = 0;
    s->take_wheel = s->take_motion = 0;
    push_takes();
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

/* A wide string as a new UTF-8 heap string, whole; NULL on failure. */
static char *to_utf8(const wchar_t *wide) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    char *out = n > 0 ? malloc((size_t)n) : NULL;
    if (out && !WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, n, NULL, NULL)) {
        free(out);
        out = NULL;
    }
    return out;
}

static void load_script(const wchar_t *path, const wchar_t *file, const char *prefix) {
    if (g_count == MAX_SCRIPTS) {
        g_api->log("more than %d scripts: the rest are left out", MAX_SCRIPTS);
        return;
    }
    Script *s = &g_scripts[g_count];
    free(s->name);  /* a slot left by a script that failed to load */
    memset(s, 0, sizeof *s);
    char *stem = to_utf8(file);
    char *name = stem ? malloc(strlen(prefix) + strlen(stem) + 1) : NULL;
    if (!name) {
        free(stem);
        g_api->log("a script's name could not be read: it is left out");
        return;
    }
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    sprintf(name, "%s%s", prefix, stem);  /* "mod/script" for a mod's scripts */
    free(stem);
    s->name = name;  /* kept for the plugin's life, as the script's slot is */
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
    char chunk[256];
    snprintf(chunk, sizeof chunk, "@%s", s->name);  /* only Lua's error messages' source name */
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
 * (BB_MODS_CONFIG; else beside the mods folder). {"disabled": ["name", ...], "order": [...]}.
 *
 * Everything here fails closed (tool-51d16fc-F1): names are kept whole, on the heap, and lists
 * have no cap. A mods.json that exists but can't be read completely loads no mod's scripts:
 * what can't be read must never count as "enabled". */
typedef struct {
    char **items;
    int n, cap;
} Names;

static int names_add(Names *list, const char *name) {
    if (list->n == list->cap) {
        const int cap = list->cap ? list->cap * 2 : 16;
        char **grown = realloc(list->items, (size_t)cap * sizeof *grown);
        if (!grown) return 0;
        list->items = grown;
        list->cap = cap;
    }
    char *copy = _strdup(name);
    if (!copy) return 0;
    list->items[list->n++] = copy;
    return 1;
}

static int names_find(const Names *list, const char *name) {
    for (int i = 0; i < list->n; ++i)
        if (!strcmp(list->items[i], name)) return i;
    return -1;
}

static void names_free(Names *list) {
    for (int i = 0; i < list->n; ++i) free(list->items[i]);
    free(list->items);
    memset(list, 0, sizeof *list);
}

static void put_utf8(char *out, size_t *w, unsigned code) {
    if (code < 0x80) out[(*w)++] = (char)code;
    else if (code < 0x800) out[(*w)++] = (char)(0xc0 | code >> 6), out[(*w)++] = (char)(0x80 | (code & 0x3f));
    else out[(*w)++] = (char)(0xe0 | code >> 12), out[(*w)++] = (char)(0x80 | ((code >> 6) & 0x3f)),
         out[(*w)++] = (char)(0x80 | (code & 0x3f));
}

/* A JSON string at *p (on its opening quote), UTF-8, into a new heap string; *p ends after the
 * closing quote. NULL when malformed. A string decodes to at most 3 bytes per input byte. */
static char *json_string(const char **p) {
    const char *s = *p;
    if (*s != '"') return NULL;
    const char *end = s + 1;
    while (*end && *end != '"') end += (*end == '\\' && end[1]) ? 2 : 1;
    if (*end != '"') return NULL;
    char *out = malloc((size_t)(end - s) * 3 + 1);
    if (!out) return NULL;
    size_t w = 0;
    for (++s; s < end;) {
        unsigned c = (unsigned char)*s++;
        if (c == '\\') {
            const char e = *s++;
            if (e == 'u') {
                unsigned code = 0;
                for (int i = 0; i < 4; ++i, ++s) {
                    const char h = *s;
                    const int digit = h >= '0' && h <= '9' ? h - '0' : (h | 0x20) >= 'a' && (h | 0x20) <= 'f' ? (h | 0x20) - 'a' + 10 : -1;
                    if (s >= end || digit < 0) {
                        free(out);
                        return NULL;
                    }
                    code = code * 16 + (unsigned)digit;
                }
                put_utf8(out, &w, code);
                continue;
            }
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : (unsigned char)e;
        }
        out[w++] = (char)c;
    }
    out[w] = 0;
    *p = end + 1;
    return out;
}

static const char *skip_space(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}

/* Skips one JSON value of any kind; NULL when malformed. */
static const char *skip_value(const char *p) {
    p = skip_space(p);
    if (*p == '"') {
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
                if (!(p = skip_value(p))) return NULL;
                p = skip_space(p);
                if (*p++ != ':') return NULL;
            }
            if (!(p = skip_value(p))) return NULL;
            p = skip_space(p);
            if (*p == ',') { p = skip_space(p + 1); continue; }
            return *p == close ? p + 1 : NULL;
        }
    }
    const char *start = p;
    while (*p && *p != ',' && *p != ']' && *p != '}' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
    return p > start ? p : NULL;
}

/* A JSON array of strings at *p into list. 0 when malformed (or out of memory). */
static int string_array(const char **p, Names *list) {
    const char *s = skip_space(*p);
    if (*s++ != '[') return 0;
    s = skip_space(s);
    if (*s == ']') {
        *p = s + 1;
        return 1;
    }
    for (;;) {
        char *name = json_string(&s);
        if (!name) return 0;
        const int added = names_add(list, name);
        free(name);
        if (!added) return 0;
        s = skip_space(s);
        if (*s == ',') { s = skip_space(s + 1); continue; }
        if (*s != ']') return 0;
        *p = s + 1;
        return 1;
    }
}

/* The whole top-level object, or 0: the caller then loads no mod's scripts. */
static int parse_mod_settings(const char *text, Names *disabled, Names *order) {
    const char *p = skip_space(text);
    if (*p++ != '{') return 0;
    p = skip_space(p);
    if (*p == '}') return *skip_space(p + 1) == 0;
    for (;;) {
        char *key = json_string(&p);
        if (!key) return 0;
        p = skip_space(p);
        if (*p++ != ':') {
            free(key);
            return 0;
        }
        int ok;
        if (!strcmp(key, "disabled")) ok = string_array(&p, disabled);
        else if (!strcmp(key, "order")) ok = string_array(&p, order);
        else ok = (p = skip_value(p)) != NULL;
        free(key);
        if (!ok) return 0;
        p = skip_space(p);
        if (*p == ',') { p = skip_space(p + 1); continue; }
        return *p == '}' && *skip_space(p + 1) == 0;
    }
}

/* 1 when the settings are known (read whole, or no mods.json at all: every mod on), 0 when a
 * mods.json exists but couldn't be read whole. */
static int load_mod_settings(const char *mods_dir, Names *disabled, Names *order) {
    wchar_t path[MAX_PATH];
    const DWORD got = GetEnvironmentVariableW(L"BB_MODS_CONFIG", path, MAX_PATH);
    if (got >= MAX_PATH) return 0;  /* a path too long to hold: unknown settings */
    if (!got) {
        wchar_t dir[MAX_PATH];
        if (!MultiByteToWideChar(CP_UTF8, 0, mods_dir, -1, dir, MAX_PATH)) return 0;
        _snwprintf(path, MAX_PATH, L"%ls\\..\\mods.json", dir);
    }
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return 1;  /* none: every mod on, as for mods.py */
    FILE *f = _wfopen(path, L"rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = size >= 0 && size < 16 * 1024 * 1024 ? malloc((size_t)size + 1) : NULL;
    int ok = 0;
    if (text && fread(text, 1, (size_t)size, f) == (size_t)size) {
        text[size] = 0;
        ok = parse_mod_settings(text, disabled, order);
    }
    free(text);
    fclose(f);
    return ok;
}

typedef struct {
    wchar_t *wide;
    char *name;
    int rank;
} Mod;

static int mod_order(const void *a, const void *b) {
    const Mod *x = a, *y = b;
    if (x->rank != y->rank) return x->rank < y->rank ? -1 : 1;
    return _wcsicmp(x->wide, y->wide);
}

/* <mods>\<mod>\scripts\*.lua for every mod folder not disabled, in the player's order (mods.json),
 * then the rest in folder-name order, as scripts/mods.py orders the mods' files. */
static void load_mods(const char *mods_dir) {
    if (!mods_dir || !*mods_dir) return;
    Names disabled = {0}, order = {0};
    if (!load_mod_settings(mods_dir, &disabled, &order)) {
        g_api->log("mods.json could not be read whole: no mod's scripts are loaded (a mod the launcher "
                   "turned off must not run)");
        names_free(&disabled);
        names_free(&order);
        return;
    }
    wchar_t dir[MAX_PATH], pattern[MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, mods_dir, -1, dir, MAX_PATH)) goto done;
    _snwprintf(pattern, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW found;
    HANDLE h = FindFirstFileW(pattern, &found);
    if (h == INVALID_HANDLE_VALUE) goto done;
    Mod *mods = NULL;
    int n = 0, cap = 0;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == L'.') continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            Mod *grown = realloc(mods, (size_t)cap * sizeof *grown);
            if (!grown) break;
            mods = grown;
        }
        Mod m = {_wcsdup(found.cFileName), to_utf8(found.cFileName), 0};
        if (!m.wide || !m.name) {  /* a name that can't be held can't be checked: left out */
            g_api->log("a mod folder name could not be read: its scripts are not loaded");
            free(m.wide);
            free(m.name);
            continue;
        }
        const int at = names_find(&order, m.name);
        m.rank = at >= 0 ? at : order.n;
        mods[n++] = m;
    } while (FindNextFileW(h, &found));
    FindClose(h);
    qsort(mods, (size_t)n, sizeof *mods, mod_order);
    for (int i = 0; i < n; ++i) {
        wchar_t scripts[MAX_PATH];
        const int fits = _snwprintf(scripts, MAX_PATH, L"%ls\\%ls\\scripts", dir, mods[i].wide) > 0;
        const DWORD attributes = fits ? GetFileAttributesW(scripts) : INVALID_FILE_ATTRIBUTES;
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (names_find(&disabled, mods[i].name) >= 0) {
            g_api->log("mod %s is disabled (mods.json): its scripts are not loaded", mods[i].name);
            continue;
        }
        char *folder = to_utf8(scripts);
        char *prefix = malloc(strlen(mods[i].name) + 2);
        if (folder && prefix) {
            sprintf(prefix, "%s/", mods[i].name);
            load_folder(folder, prefix);
        }
        free(folder);
        free(prefix);
    }
    for (int i = 0; i < n; ++i) {
        free(mods[i].wide);
        free(mods[i].name);
    }
    free(mods);
done:
    names_free(&disabled);
    names_free(&order);
}
/* --- the keyboard and mouse ---------------------------------------------------------------------- */

/* Read once a frame, before any script's callbacks, and shared by every script (the host's input
 * has one reader: the presses are since the last read). g_prev is the frame before's keys, so a
 * press is "down now, up then". Before the first frame (while scripts load) nothing is down. */
static BbInput g_in, g_prev_in;

static void read_input(void) {
    g_prev_in = g_in;
    if (g_api->input(&g_in)) memset(&g_in, 0, sizeof g_in);  /* headless: nothing held */
}

/* Key names, as SDL names its scancodes (lower case): "a".."z", "0".."9", "f1".."f12", "space",
 * "enter", "escape", "tab", "backspace", the arrows, "insert", "delete", "home", "end", "pageup",
 * "pagedown", "lshift", "rshift", "lctrl", "rctrl", "lalt", "ralt", "kp0".."kp9". A number is a
 * scancode as is. */
static int key_code(lua_State *L, int at) {
    if (lua_type(L, at) == LUA_TNUMBER) {
        const lua_Integer code = luaL_checkinteger(L, at);
        luaL_argcheck(L, code > 0 && code < 512, at, "a scancode is 1..511");
        return (int)code;
    }
    const char *name = luaL_checkstring(L, at);
    char k[16];
    size_t n = 0;
    for (; name[n] && n < sizeof k - 1; ++n) k[n] = (char)((name[n] >= 'A' && name[n] <= 'Z') ? name[n] + 32 : name[n]);
    k[n] = 0;
    if (!name[n]) {
        if (n == 1 && k[0] >= 'a' && k[0] <= 'z') return 4 + (k[0] - 'a');
        if (n == 1 && k[0] >= '1' && k[0] <= '9') return 30 + (k[0] - '1');
        if (n == 1 && k[0] == '0') return 39;
        /* f1..f12: digits only after the f, so a typo ("f1x") is an error, not F1 (tool-f2ab4ec-F1). */
        if (k[0] == 'f' && n >= 2 && n <= 3 && k[1] >= '1' && k[1] <= '9' && (n == 2 || (k[2] >= '0' && k[2] <= '9'))) {
            const int f = atoi(k + 1);
            if (f <= 12) return 57 + f;
        }
        if (!strncmp(k, "kp", 2) && n == 3 && k[2] >= '0' && k[2] <= '9') return k[2] == '0' ? 98 : 89 + (k[2] - '1');
        static const struct { const char *name; int code; } named[] = {
            {"enter", 40}, {"return", 40}, {"escape", 41}, {"backspace", 42}, {"tab", 43}, {"space", 44},
            {"insert", 73}, {"home", 74}, {"pageup", 75}, {"delete", 76}, {"end", 77}, {"pagedown", 78},
            {"right", 79}, {"left", 80}, {"down", 81}, {"up", 82},
            {"lctrl", 224}, {"lshift", 225}, {"lalt", 226}, {"rctrl", 228}, {"rshift", 229}, {"ralt", 230},
        };
        for (size_t i = 0; i < sizeof named / sizeof *named; ++i)
            if (!strcmp(k, named[i].name)) return named[i].code;
    }
    return luaL_argerror(L, at, lua_pushfstring(L, "no key named '%s'", name));
}

/* bb.input.down(key) -> true while held (the window focused). */
static int l_input_down(lua_State *L) {
    lua_pushboolean(L, g_in.key[key_code(L, 1)] != 0);
    return 1;
}

/* bb.input.pressed(key) -> true in the frame it went down. */
static int l_input_pressed(lua_State *L) {
    const int code = key_code(L, 1);
    lua_pushboolean(L, g_in.key[code] && !g_prev_in.key[code]);
    return 1;
}

/* bb.input.playing() -> true in the world with no menu or text box up, the window focused. */
static int l_input_playing(lua_State *L) {
    lua_pushboolean(L, g_in.playing != 0);
    return 1;
}

/* bb.input.mouse() -> {left, right, middle, back, forward (held), clicked = {same, this frame},
 * dx, dy (motion this frame while playing), wheel (notches, up positive)}. */
static int l_input_mouse(lua_State *L) {
    static const char *const names[] = {"left", "right", "middle", "back", "forward"};
    lua_createtable(L, 0, 9);
    lua_createtable(L, 0, 5);
    for (int i = 0; i < 5; ++i) {
        lua_pushboolean(L, (g_in.buttons >> i) & 1);
        lua_setfield(L, -3, names[i]);
        lua_pushboolean(L, (g_in.pressed >> i) & 1);
        lua_setfield(L, -2, names[i]);
    }
    lua_setfield(L, -2, "clicked");
    lua_pushnumber(L, g_in.dx);
    lua_setfield(L, -2, "dx");
    lua_pushnumber(L, g_in.dy);
    lua_setfield(L, -2, "dy");
    lua_pushnumber(L, g_in.wheel);
    lua_setfield(L, -2, "wheel");
    return 1;
}

/* bb.input.on_key(key, fn): fn() in each frame the key goes down while playing (a hotkey: not
 * while typing in a text box or in a menu). */
static int l_input_on_key(lua_State *L) {
    const int code = key_code(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_getfield(L, LUA_REGISTRYINDEX, "bb_keys");
    lua_createtable(L, 2, 0);
    lua_pushinteger(L, code);
    lua_rawseti(L, -2, 1);
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, 2);
    lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
    lua_pop(L, 1);
    return 0;
}

/* The host has one taker (input_take replaces the last call): it gets every running script's
 * takes together. 0 when the host accepted. */
static int push_takes(void) {
    uint16_t codes[512];
    size_t n = 0;
    uint32_t buttons = 0;
    int wheel = 0, motion = 0, any = 0;
    for (int i = 0; i < g_count; ++i) {
        const Script *s = &g_scripts[i];
        if (!s->on) continue;
        buttons |= s->take_buttons;
        wheel |= s->take_wheel;
        motion |= s->take_motion;
        any |= s->take_buttons || s->take_wheel || s->take_motion;
    }
    for (int code = 1; code < 512; ++code)
        for (int i = 0; i < g_count; ++i)
            if (g_scripts[i].on && (g_scripts[i].take_keys[code / 32] >> (code % 32) & 1)) {
                codes[n++] = (uint16_t)code;
                break;
            }
    static int took;  /* never called input_take with nothing to give back: leave other plugins' alone */
    if (!n && !any && !took) return 0;
    took = n || any;
    return g_api->input_take(codes, n, buttons, wheel, motion);
}

/* bb.input.take{keys = {...}, buttons = {"left", ...}, wheel = bool, motion = bool} -> true when
 * taken: while playing, these no longer reach the game (the script still reads them). Replaces
 * the script's last take; bb.input.take() gives everything back. */
static int l_input_take(lua_State *L) {
    Script *s = script_of(L);
    uint32_t keys[16] = {0}, buttons = 0;
    int wheel = 0, motion = 0;
    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_getfield(L, 1, "keys");
        if (!lua_isnil(L, -1)) {
            luaL_argcheck(L, lua_istable(L, -1), 1, "keys is a list of key names");
            const int list = lua_gettop(L);
            for (lua_Integer i = 1, len = (lua_Integer)lua_rawlen(L, list); i <= len; ++i) {
                lua_rawgeti(L, list, i);
                const int code = key_code(L, lua_gettop(L));
                keys[code / 32] |= 1u << (code % 32);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        lua_getfield(L, 1, "buttons");
        if (!lua_isnil(L, -1)) {
            static const char *const names[] = {"left", "right", "middle", "back", "forward", NULL};
            luaL_argcheck(L, lua_istable(L, -1), 1, "buttons is a list of mouse button names");
            const int list = lua_gettop(L);
            for (lua_Integer i = 1, len = (lua_Integer)lua_rawlen(L, list); i <= len; ++i) {
                lua_rawgeti(L, list, i);
                buttons |= 1u << luaL_checkoption(L, lua_gettop(L), NULL, names);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        lua_getfield(L, 1, "wheel");
        wheel = lua_toboolean(L, -1);
        lua_getfield(L, 1, "motion");
        motion = lua_toboolean(L, -1);
        lua_pop(L, 2);
    }
    memcpy(s->take_keys, keys, sizeof keys);
    s->take_buttons = buttons;
    s->take_wheel = wheel;
    s->take_motion = motion;
    lua_pushboolean(L, push_takes() == 0);
    return 1;
}

/* The hotkeys pressed this frame, each script's in the order registered. */
static void dispatch_keys(void) {
    if (!g_in.playing) return;
    for (int i = 0; i < g_count; ++i) {
        Script *s = &g_scripts[i];
        if (!s->on) continue;
        lua_getfield(s->L, LUA_REGISTRYINDEX, "bb_keys");
        const lua_Integer n = (lua_Integer)lua_rawlen(s->L, -1);
        for (lua_Integer k = 1; k <= n && s->on; ++k) {
            lua_rawgeti(s->L, -1, k);
            lua_rawgeti(s->L, -1, 1);
            const lua_Integer code = lua_tointeger(s->L, -1);
            lua_pop(s->L, 1);
            if (code > 0 && code < 512 && g_in.key[code] && !g_prev_in.key[code]) {
                lua_rawgeti(s->L, -1, 2);
                lua_remove(s->L, -2);
                call(s, 0);
            } else {
                lua_pop(s->L, 1);
            }
        }
        lua_settop(s->L, 0);
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
    read_input();
    dispatch_keys();
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
    g_online = env_is("BB_ONLINE", "1") && !env_is("BB_COOP", "1");
    if (g_online) api->log("online play: scripts run, but their calls that change the game do nothing");
    g_loading_scripts = 1;
    load_folder(api->plugin_dir("lua"), "");
    load_mods(api->mods_dir ? api->mods_dir() : NULL);
    g_loading_scripts = 0;
    int on = 0;
    for (int i = 0; i < g_count; ++i) on += g_scripts[i].on;
    api->log("%d script(s) running, %d stopped", on, g_count - on);
    if (!g_count) return 1;  /* nothing to run: unload */
    return api->on_frame(on_frame, NULL);
}

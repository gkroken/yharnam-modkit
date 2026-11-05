# Lua scripts

`bb_lua.dll` (`plugins/lua`) runs Lua 5.4 scripts as mods, with no compiler needed. It's a native
plugin like any other (`port/docs/PLUGINS.md`); each `.lua` file is a script with its own Lua
state.

## Installing

1. `bash tools/build-plugins.sh --install` copies `bb_lua.dll` into `port\plugins\`, or put the
   DLL in a package's `plugins\` folder.
2. Put scripts in `<user>\plugins\lua\` (`port\user\plugins\lua\` in a source tree). They load
   in name order when the game starts.

The log shows each script loading (`Plugin lua: <script>: loaded`), its own lines, and why any
stopped.

## Scripts in a mod

A mod can ship scripts beside its files: `mods\<mod>\scripts\*.lua`.
- **Names:** they're named `<mod>/<file>` (`mymod/main`), so two mods can each have a
  `main.lua`. The name is also their `plugins.ini` section: `[mymod/main]`.
- **Order:** they load after `<user>\plugins\lua\`, mods in the player's order (the launcher's
  Mods page, `mods.json`), then any others in folder-name order, as the port orders mods' files.
- **Disabled mods:** a mod disabled in the launcher has its scripts left out too. That's the
  same `mods.json` the port's mod merging reads (`BB_MODS_CONFIG`).
- **Script-only mods:** a mod made only of scripts is fine. The port's mod merging lists it as
  having no game files and carries on.

`plugins/lua/examples/` has two scripts to start from: `flag_buff.lua` (a SpEffect each time a
chosen flag turns on) and `weapon_tweak.lua` (one param field changed by name).

## The `bb` table

Every call goes through the native plugin API (`port/docs/PLUGINS.md`), with the same rules: the
1.09 build, and writes on the main thread. Every callback below runs on the main thread, so
writes from callbacks are fine. A call that can't answer returns `nil` (reads) or `false` (writes).

**The script itself:**

| Call | What it does |
|---|---|
| `bb.log(...)` | One log line: the arguments as text, separated by spaces. `print` does the same. |
| `bb.config(key)` | The script's option from `plugins.ini`, as text, or `nil`. The section is the script's file name: `[hello] greeting = hi` for `hello.lua`. |

**Callbacks** (a script may register several of each):

| Call | What it does |
|---|---|
| `bb.on_frame(fn)` | `fn()` once a frame. |
| `bb.on_world_load(fn)` | `fn(block)` when the player is placed in a map: a load or a warp. |
| `bb.flag.on(fn)` | `fn(id, on)` for every event flag change, at the start of the next frame. Register while the script loads. |

**The world's state:**

| Call | What it does |
|---|---|
| `bb.flag.get(id)` | `true` or `false`, or `nil` while the flag's block doesn't exist. |
| `bb.flag.set(id, on)` | Sets an event flag; `true` when written. |
| `bb.param.get(table, id, field)` | A param field as text, or `nil`: `bb.param.get("EquipParamWeapon", 2000000, "attackBasePhysics")`. |
| `bb.param.set(table, id, field, value)` | Writes it (a number or text, checked against the field's range); `true` when written. |
| `bb.param.ids(table)` | The table's row ids, in order, or `nil`. |

**The player and characters:**

| Call | What it does |
|---|---|
| `bb.player.stat(name)` | `level`, `echoes`, `hp`, `vitality`... as a number, or `nil`. |
| `bb.player.stat(name, value)` | Sets it; `true` when written. |
| `bb.player.position()` | `x, y, z, yaw`, or `nil` with no player. |
| `bb.player.block()` | The map block (`0x18010000` is the Hunter's Dream), or `nil`. |
| `bb.chr.list()` | The world's characters, the player first. Each entry has `ins`, `handle`, `npc`, `model`, `hp`, `max_hp`, `team`, `type`, `player`, `x`, `y`, `z`. Use the list within the frame you got it in. |
| `bb.effect.apply(id[, chr])` | Applies a SpEffectParam row to the player, or to a character (`entry.ins` from `bb.chr.list()`); `true` when applied. |
| `bb.effect.has(id[, chr])` | `true` while it's on. |

**Text over the game:**

| Call | What it does |
|---|---|
| `bb.message(text[, seconds])` | A banner (3 s by default). |
| `bb.text(text, seconds, x, y[, scale[, align]])` | A line placed at `x, y` (fractions of the window). `align` is `"left"`, `"centre"` or `"right"`, optionally with `" bottom"`. |

## What a script can't do

Scripts get base Lua, `table`, `string`, `math`, `utf8`, `coroutine`, and `os.clock`, `os.time`
and `os.date`. They can't read or write files, run programs, load code (`load`, `loadfile`,
`dofile`, `require`) or bytecode (`string.dump`, binary chunks), or reach `debug`.

## When a script goes wrong

One script's trouble stops only that script, with a log line and a banner over the game
(`Lua script <name> stopped`):
- an error, including a syntax error at load;
- more than 20 million Lua instructions in one call, an endless loop for example;
- more than 64 MiB of memory.

The other scripts and the game carry on.

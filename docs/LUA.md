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

## The `bb` table (so far)

| Call | What it does |
|---|---|
| `bb.log(...)` | One log line: the arguments as text, separated by spaces. `print` does the same. |
| `bb.on_frame(fn)` | `fn()` once a frame, on the game's main thread. A script may register several. |
| `bb.config(key)` | The script's option from `plugins.ini`, as text, or `nil`. The section is the script's file name: `[hello] greeting = hi` for `hello.lua`. |

The game-state calls (flags, params, the player, effects, characters, messages) come next; see
`docs/WORKPLAN.md`, Phase 4.

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

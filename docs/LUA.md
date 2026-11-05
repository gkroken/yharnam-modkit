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
  same `mods.json` the port's mod merging reads (`BB_MODS_CONFIG`). If that file exists but
  can't be read whole (cut off, or not the launcher's shape), no mod's scripts load, and the log
  says so. Your own scripts in `<user>\plugins\lua\` still load. Names of any length work.
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
| `bb.replace_text(address, text)` | While the script loads (its top level) only: the game's UTF-16 string at a Binary Ninja address of the 1.09 image shows `text` instead, wherever the code and data name it. Returns the references moved; 0 means nothing names that string. The first replacement of a string stays. `bb.replace_text(0x4d686cc, "maptpf:/%s/%s_9998.tpf")` |
| `bb.loading()` | `true` while a loading screen is up: a load, a warp, a death. `bb.on_world_load` runs while it is still up. |
| `bb.player.block()` | The map block (`0x18010000` is m24_01, Central Yharnam), or `nil`. |
| `bb.chr.list()` | The world's characters, the player first. Each entry has `ins`, `handle`, `npc`, `model`, `hp`, `max_hp`, `team`, `type`, `player`, `x`, `y`, `z`. Use the list within the frame you got it in. |
| `bb.effect.apply(id[, chr])` | Applies a SpEffectParam row to the player, or to a character (`entry.ins` from `bb.chr.list()`); `true` when applied. |
| `bb.effect.has(id[, chr])` | `true` while it's on. |

**Text over the game:**

| Call | What it does |
|---|---|
| `bb.message(text[, seconds])` | A banner (3 s by default). |
| `bb.text(text, seconds, x, y[, scale[, align]])` | A line placed at `x, y` (fractions of the window). `align` is `"left"`, `"centre"` or `"right"`, optionally with `" bottom"`. |

**The keyboard and mouse** (read once a frame, before the callbacks; the window must have the
focus):

| Call | What it does |
|---|---|
| `bb.input.down(key)` | `true` while the key is held. `key` is a name (`"f5"`, `"g"`, `"space"`, `"lshift"`, `"up"`, `"kp1"`...) or an SDL scancode. |
| `bb.input.pressed(key)` | `true` in the frame the key went down. |
| `bb.input.on_key(key, fn)` | A hotkey: `fn()` in the frame the key goes down, while playing (not in a menu or a text box). |
| `bb.input.take{keys = {...}, buttons = {...}, wheel = true, motion = true}` | While playing, these no longer reach the game: a hotkey on G without the gestures menu opening. The script still reads them. Buttons are `"left"`, `"right"`, `"middle"`, `"back"`, `"forward"`; `motion` keeps the mouse from turning the camera. Replaces the script's last take; `bb.input.take()` gives everything back, and so does the script stopping. `true` when taken. |
| `bb.input.playing()` | `true` in the world with no menu or text box up. |
| `bb.input.mouse()` | `{left, right, middle, back, forward}` held, `clicked` (the same names, this frame), `dx`, `dy` (motion this frame), `wheel`. |

Keys still do what they do in the game too, unless a script takes them. Otherwise pick a hotkey
nothing else uses: the port's default
keys leave F1 to F8, F11 and F12 free. F9 and F10 can open the map, and `bbport.ini`'s `key_*`
lines can rebind the game's keys.

## What a script can't do

- **Warp the player** (yet). The host's warps are for native plugins the player turned on, flagged
  as changing the game. Lua scripts aren't, so far.
- **Change the game in online play.** With `BB_ONLINE=1` (not co-op), `bb.flag.set`,
  `bb.param.set`, `bb.player.stat(name, value)` and `bb.effect.apply` do nothing and return
  false; the log says so once per script. Reading, watching and text still work. This matches
  the plugin host, which leaves native plugins that change the game out of online play.

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

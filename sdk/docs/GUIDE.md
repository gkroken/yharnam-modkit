# Modding Bloodborne PC with code: the guide

Mods for the yharnam-pc port usually replace files. With this SDK a mod can also run code while
the game plays: **Lua scripts** (plain text, no compiler) or **native plugins** (C DLLs). This
guide takes you from nothing to a mod you can share. The references are `LUA.md` (every Lua call)
and `PLUGINS.md` (the native API as this host provides it, plugins.ini, the rules).

## 1. What you need

- **A build of yharnam-pc with the plugin host.** It's in the `plugins` branch of
  <https://github.com/gkroken/yharnam-pc>. Released yharnam-pc packages don't have it yet: a
  plugin or script put beside one of those does nothing. Build it from source
  (`packaging/windows/README.md` in that repository: MSYS2 CLANG64, then `bash build.sh`).
- **The game:** CUSA03173, version 1.09, dumped from your own console. Everything here is for 1.09
  only.
- **For native plugins:** clang for Windows with the MinGW ABI, CMake and Ninja (MSYS2's CLANG64
  has all three). Lua scripts need nothing.

## 2. Where things go

`<install>` is the port's folder (the one with `run.py`); `<user>` is beside your saves,
`<install>\user` unless the launcher says otherwise.

| What | Where |
|---|---|
| Native plugins (`*.dll`) | `<install>\plugins\` or `<user>\plugins\` (a file in `<user>` replaces one of the same name) |
| The Lua host, `bin\bb_lua.dll` from this SDK | the same `plugins` folder |
| Your own Lua scripts | `<user>\plugins\lua\*.lua` |
| A mod's Lua scripts | `mods\<mod>\scripts\*.lua`, named `<mod>/<file>` |
| Switches and options | `<user>\plugins.ini` (the launcher's Mods & Patches > Plugins and scripts page sets the switches) |
| What happened | the launcher's Log page, or `<user>\last_run.log` |

A file renamed to turn it off (`x.dll_off`, `x.lua_off`) stays off.

## 3. A first Lua mod

Make a mod folder with a script in it, `mods\first_light\scripts\main.lua`:

```lua
local banner = bb.config("banner") or "The hunt begins"

bb.on_world_load(function(block)
  bb.message(banner)
  bb.log(("world loaded: block 0x%08x, %d echoes"):format(block, bb.player.stat("echoes")))
end)

bb.input.on_key("F6", function()
  local x, y, z = bb.player.position()
  bb.log(("standing at %.1f, %.1f, %.1f"):format(x, y, z))
end)
```

Start the game and load your character: the banner shows, and the log has
`Plugin lua: first_light/main: world loaded: ...`. Its option lives in plugins.ini, in a section
named after the script:

```ini
[first_light/main]
banner = Fear the old blood
```

`examples\lua\` has more: a buff on a flag, a weapon param changed by name, a banner per lamp lit.

## 4. Changing and testing it

- **Reload without restarting:** `[lua] reload_key = f8` in plugins.ini. F8 closes every script and
  loads them again from disk, mods' included. Two things only the game's start can do don't
  reload (`LUA.md`, Hot reload).
- **Read the log.** Every line of yours starts `Plugin lua: <script>:`. A script that errors, loops
  or uses too much memory stops alone, with a line saying why and a banner; the game carries on.
  `Plugins:` lines say which plugins loaded and why any didn't.
- **Test it in the world without touching your save:** `tools\game-world.ps1` starts the game,
  loads your character, presses keys, takes a picture, prints the plugin lines, and puts the save
  and every file back:

  ```
  powershell -ExecutionPolicy Bypass -File tools\game-world.ps1 -Install <install> `
      -Scripts mods\first_light\scripts\main.lua -Keys F6 -Shot shot.png
  ```
  `-Plugins` adds DLLs, `-Ini` a plugins.ini for the run. A script given with `-Scripts` runs from
  `<user>\plugins\lua`, so it's named after its file for that run (`main`, its section `[main]`),
  not `first_light/main`. Two files with one name (two mods' `main.lua`) can't go in one run: the
  runner says so and stops.

## 5. Finding what to change

The API names the game's things; you still need to know which thing.

- **Params** (weapons, armour, effects, enemies' stats): `bb.param.get/set(table, id, field)` use
  the game's own table and field names. After one start, `<install>\out\paramdefs.txt` lists every
  table and field. `bb.param.ids(table)` gives a table's row ids.
- **Event flags** (bosses killed, doors opened, NPC steps, lamps lit) are numbers. Watch them
  change while you play: `bb.flag.on(function(id, on) bb.log(id, on) end)` logs every flag the
  game flips, so do the thing in game and read the log. The game's event scripts (EMEVD), opened
  with DarkScript3 or Soulstruct, say which flag an event sets.
- **Where you are:** `bb.player.block()` (the map, e.g. `0x18010000` is Central Yharnam) and
  `bb.player.position()`.
- **Characters:** `bb.chr.list()` gives everyone loaded, with their HP, model and position.
- **SpEffects** (buffs and states) are rows of `SpEffectParam`: `bb.effect.apply(id)` on the player.

## 6. Native plugins

Use C when Lua can't do it: hooks on the game's functions, calls into the game, raw memory.

- **Start from `template\`** (`cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=<sdk>/cmake/bb-clang-mingw.cmake -DBB_SDK_DIR=<sdk>`),
  or an example in `examples\c\` (`frame_hook` has a hook and a patch).
- **The lifecycle:** `bb_plugin_info` (name, version, flags), `bb_plugin_init` (log and config
  only), `bb_plugin_image` (the game is loaded: register callbacks; hooks only here, they're
  sealed after), then callbacks such as `on_frame` on the game's main thread, the one place to
  change the game.
- **Flag a plugin that changes the game** `BB_PLUGIN_GAMEPLAY`: online play leaves it out, as it
  does cheats. `BB_PLUGIN_OPT_IN` keeps it off until the player turns it on.

### Addresses

- **They're Binary Ninja's for the 1.09 `eboot.bin`,** with the image at its preferred base
  `0x400000`. `read`, `write`, `patch` and `hook` take those addresses; `guest_addr()` gives the
  runtime address of one. `read()` also takes runtime addresses of the game's heaps (pointers you
  read out of the game).
- **Check before you change.** `eboot_is_109()` first; `patch` takes the bytes you expect there,
  and `hook` the function's first bytes: a mismatch is refused and logged, never applied.
- **Take a function's first bytes from memory** (`read()` in `bb_plugin_image`), not from a
  disassembly of `eboot.elf`: the port rewrites about 3,500 function entries
  (`PLUGINS.md`, Hooks).
- **Name every address** in your code: a constant with what it is and how you know (the function's
  name in your Binary Ninja database, bbhost's engine headers, a debugger). The next person to
  read it, or you after the next game update, will need it.

## 7. Sharing a mod

- **A Lua mod** is a folder: `mods\<mod>\scripts\*.lua`, beside any files it replaces. Players drop
  it in `mods\`; the launcher's Installed mods page turns it on and off, and its order.
- **A plugin** is its DLL (and a folder of its own for any DLLs it needs). Players put it in
  `<user>\plugins\`.
- **Ship your own code only:** no game files and no bytes copied from the game. To recognise game
  code, compare its length and a hash, not a copy of it.
- **The license:** the plugin API is GPL-3.0-or-later, so a plugin built against it is shared under
  the GPL with its source. Lua scripts are yours to license, but sharing them openly helps everyone.

## 8. When it doesn't work

| You see | It means |
|---|---|
| No `Plugin lua:` line at all | `bb_lua.dll` isn't in a `plugins` folder, or this build has no plugin host |
| `Plugins: <name> (...): left out, off in plugins.ini` | turned off: `[plugins] <name> = 1`, or the launcher's switch |
| `left out, off until turned on in plugins.ini (opt-in)` | an opt-in plugin: turn it on |
| `left out, changes the game, and this is online play` | online play leaves gameplay plugins out |
| `Plugin lua: <script>: stopped: <why>` | your script's error, with its line number |
| `mod <mod> is disabled (mods.json)` | the mod is off in the launcher, and its scripts with it |
| A hook or patch refused | the bytes there aren't what you passed: wrong address, not 1.09, or bytes from `eboot.elf` |

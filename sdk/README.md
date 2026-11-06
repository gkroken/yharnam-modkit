# Bloodborne PC plugin SDK

Everything needed to write mods that run code in Bloodborne on the
[yharnam-pc](https://github.com/Jgamrot/yharnam-pc) PC port: native plugins in C, and Lua
scripts with no compiler at all. The game must be CUSA03173 version 1.09.

**Start with `docs/GUIDE.md`.** Using a coding agent (Claude Code and the like)? It reads
`AGENTS.md` (`CLAUDE.md` points there): the loop of writing a mod, running it in the world and
reading the log, and the rules.

**You need a build of yharnam-pc with the plugin host:** the `plugins` branch of
<https://github.com/gkroken/yharnam-pc>, built from source. Released yharnam-pc packages don't
have it yet, so a plugin or script put beside one of those does nothing.

| Path | What |
|---|---|
| `include/bbhost_plugin.h` | The plugin API (bbhost's, by droogie). Every call is documented in it. |
| `bin/bb_lua.dll` | The Lua host: put it in the game's `plugins` folder to run Lua scripts. |
| `docs/` | `GUIDE.md` (the modder guide: from nothing to a mod you can share), `LUA.md` (every Lua call), `PLUGINS.md` (what the host provides, plugins.ini, the rules). |
| `AGENTS.md`, `CLAUDE.md` | Instructions for coding agents. |
| `template/` | A plugin of your own to start from: `my_plugin.c` and its `CMakeLists.txt`. |
| `examples/c/` | Example plugins: `hello`, `frame_hook` (a hook and a patch), `flag_buff`, `weapon_tweak`. |
| `examples/lua/` | Example scripts: a flag-triggered buff, a weapon tweak, a lamp notification. |
| `examples/mods/` | Example mods: `event_flag`, an event added to Central Yharnam's event script (data, no code) and the Lua script that shows it running. |
| `tools/game-world.ps1` | Runs the game with your plugins, scripts and plugins.ini, loads your character, presses keys, takes a picture and prints the plugin log lines; puts the save and every file back afterwards. For you, or a coding agent, to test a mod in the world: `-Install <port tree>`, `-Mods` for whole mod folders (see the script's header). |
| `cmake/` | `bb-clang-mingw.cmake` (the toolchain file) and `BloodbornePlugin.cmake` (`bb_add_plugin`). |
| `src/bb_lua/` | The source of `bin/bb_lua.dll`: `bb_lua.c` and Lua 5.4.7 (with its one patch, in `lstrlib.c`). |
| `VERSION` | Which plugin API and which source revision this package was made from. |

## Where things go in the game

- **Plugins** (`*.dll`): `plugins\` in the port's folder (the one with `run.py`), or `plugins\`
  beside your saves. They load in name order when the game starts.
- **Lua scripts:** `bb_lua.dll` in a `plugins` folder, and scripts in `plugins\lua\` beside your
  saves, or in a mod: `mods\<mod>\scripts\*.lua`.
- **Turning things off:** `BB_PLUGINS=0` loads no plugins; `[plugins] <name> = 0` in
  `plugins.ini` beside your saves turns one off. Online play leaves out every plugin that changes
  the game.
- **The log:** the launcher's Log page, or `last_run.log` beside your saves. Each plugin's lines
  start `Plugin <name>:`, and a `Plugins:` line says why any was left out.

## A first Lua script

Copy `bin/bb_lua.dll` into the game's `plugins` folder, and save this as `plugins\lua\hello.lua`
beside your saves:

```lua
bb.on_world_load(function(block)
  bb.message(("Welcome to block 0x%08x"):format(block))
end)
```

`docs/LUA.md` lists every call; `examples/lua/` has more.

## A first plugin

You need clang for Windows with the MinGW ABI, CMake and Ninja. The tested setup is
[MSYS2](https://www.msys2.org/) with its CLANG64 toolchain:

```sh
pacman -S mingw-w64-clang-x86_64-toolchain mingw-w64-clang-x86_64-cmake mingw-w64-clang-x86_64-ninja
```

Then, from the MSYS2 CLANG64 shell (or any shell with `C:\msys64\clang64\bin` on `PATH`):

```sh
cp -r template ~/my_plugin && cd ~/my_plugin
cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=<sdk>/cmake/bb-clang-mingw.cmake -DBB_SDK_DIR=<sdk>
cmake --build build
```

Builds are optimized (Release) unless you pass `-DCMAKE_BUILD_TYPE=Debug`, and C is C11 with
GNU extensions (`-std=gnu11`, so `<math.h>` has `M_PI`); `-DCMAKE_C_EXTENSIONS=OFF` makes it
strict C11. The SDK looks for
MSYS2's clang first and says which it took; `-DBB_CLANG=<path>` picks another (it must target
MinGW: the official LLVM installer's clang targets MSVC and won't link these).

Copy `build/my_plugin.dll` into the game's `plugins` folder, start the game, load a character and
press F6. To build every example: `cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=cmake/bb-clang-mingw.cmake`
here, then `cmake --build build`.

**Why not MSVC:** a function the game calls directly needs the System V calling convention
(`BB_GUEST_ABI`), which MSVC can't produce. Ordinary callbacks (`on_frame` and the rest) are plain
C, but the SDK is built and tested with clang only.

## The rules a plugin lives by

- **Addresses are the 1.09 game's,** as Binary Ninja shows them (the image at 0x400000). Check
  `eboot_is_109()`, and pass the bytes you expect to `patch` and `hook`: a mismatch is refused,
  never applied.
- **Hooks only in `bb_plugin_image`.** The host seals them after the start; later calls fail.
- **Change the game from `on_frame`** (the main thread), and the other callbacks that say so.
- **Flag a plugin that changes the game** `BB_PLUGIN_GAMEPLAY` in its `bb_plugin_info`, so online
  play leaves it out.
- **Ship your own code only.** No game files or bytes in a mod: compare against them by length
  and hash instead.

## License

GPL-3.0-or-later (`LICENSE`): a plugin built against this API is a work based on it, so share its
source the same way. `bin/bb_lua.dll` includes Lua 5.4.7 (MIT, `src/bb_lua/lua-5.4.7/LICENSE`);
its whole source is in `src/bb_lua/`. The port's source, plugin host included, is at
<https://github.com/gkroken/yharnam-pc> (branch `plugins`).

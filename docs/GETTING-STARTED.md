# Getting started: mods that run code

This project adds two ways to change Bloodborne (on the yharnam-pc PC port) while it runs:
- **Lua scripts:** plain text files. React to event flags, change params by field name, read the
  player, apply effects, put text on screen. No compiler.
- **Native plugins:** DLLs written in C against bbhost's plugin API. Everything Lua can do,
  plus hooks on game functions, calls into the game and raw memory.

Both work beside ordinary file mods, never change your game files, and can be turned off.

## 1. Build and run the port from source

Once:
1. Install MSYS2 and its CLANG64 packages (`port/packaging/windows/README.md`).
2. Build the port: `bash build.sh` in `port/`, in the MSYS2 CLANG64 shell.

Then:

```bash
bash tools/build-plugins.sh            # the examples, the Lua host (bb_lua.dll) and the tool's tests
BB_GAME_DIR=C:/games/Bloodborne/game <python.org python> port/run.py
```

`bash tools/test-port.sh` runs every test against the recorded baseline.

## 2. A first Lua script

1. Copy `out/plugins/bb_lua.dll` into `port/plugins/` (`build-plugins.sh --install` copies every
   example too).
2. Make `port/user/plugins/lua/hello.lua`:

```lua
bb.log("hello from Lua")

bb.on_world_load(function(block)
  bb.message(("Welcome to block 0x%08x"):format(block))
end)

local frames = 0
bb.on_frame(function()
  frames = frames + 1
  if frames % 600 == 0 then
    bb.log("echoes:", bb.player.stat("echoes"))
  end
end)
```

3. Start the game. The terminal running `run.py` shows `Plugin lua: hello: hello from Lua`
   (started from the launcher, it's on the Log page and in `last_run.log` beside your saves), and
   the banner appears once your character loads.

Every call is listed in `docs/LUA.md`. To ship scripts with a mod, put them in
`mods/<your mod>/scripts/`; they're named `<your mod>/<file>`. Ready-made scripts are in
`plugins/lua/examples/`.

## 3. A first native plugin

Start from `plugins/examples/hello/hello.c` (the smallest plugin) or `frame_hook/frame_hook.c` (a
hook and a patch). Add a folder under `plugins/examples/`, and `build-plugins.sh` builds it.

For a plugin of your own outside this tree, use the SDK: `bash tools/make-sdk.sh` makes
`out/sdk/bloodborne-plugin-sdk-api<N>.zip`, whose `template/` builds with CMake and Ninja
(its README says how). That zip is what a modder without this repository needs.

- **The API:** `port/include/bbhost_plugin.h`.
- **What this host provides,** and how it differs from bbhost: `port/docs/PLUGINS.md`.
- **Build with clang or MinGW:** callbacks the game calls directly need the System V calling
  convention, which MSVC can't produce.
- **Hooks:** a function's prologue bytes come from memory (`read()` in `bb_plugin_image`), not
  from `eboot.elf`. They differ for about 3,500 functions (PLUGINS.md explains why).

## 4. When something goes wrong

- **The log:** the terminal running `run.py`; from the launcher, its Log page and `last_run.log`
  beside your saves. Every `Plugins:` line says what loaded or was left out, and why. A plugin's
  own lines start `Plugin <name>:`.
- **A Lua script that errors, loops forever or uses too much memory** is stopped alone, with a
  banner. The game and the other scripts carry on.
- **Turning things off:**
  - `BB_PLUGINS=0` loads no plugins;
  - `[plugins] <name> = 0` in `port/user/plugins.ini` turns one off;
  - online play leaves out every native plugin that changes the game, and Lua scripts' calls
    that change it do nothing (`docs/LUA.md`).

## Where things are

| Path | What |
|---|---|
| `port/` | The game port (our fork). The plugin host is `src/plugin_*.c`. |
| `plugins/examples/` | C example plugins. |
| `plugins/lua/` | The Lua host, and example scripts. |
| `docs/` | This guide, `LUA.md`, the workplan, and the research notes. |
| `tools/` | The test gate, the plugin build, and the in-world test runner. |

# Research: Bloodborne PC modding as of 2026-11

## The runtime

`C:\games\Bloodborne\bbport-windows` holds **yharnam-pc** (Jgamrot), a fork of Supermedo's
`bloodborne_pc`, which builds on deadinside28's **bbport**. Its renderer and shader recompiler
come from shadPS4. The game's 1.09 `eboot.bin` runs natively on the CPU; this is not
emulation. Graphics are translated to Vulkan. Game dump: `C:\games\Bloodborne\game`
(CUSA03173, GOTY, 1.09).

## What yharnam-pc already offers modders

- **File mods.** Folders in `mods\`, merged through links, so no game file changes
  (`scripts/mods.py`). They cover every `dvdroot_ps4` folder: `event` (EMEVD),
  `script/talk` (ESD), `script/ai` (Lua/HKS), `param`, `chr`, `map`, `menu`, and the rest.
  Mod order and disabled mods are kept in a JSON config.
- **Byte patches.** `patches/Bloodborne.xml` (shadPS4 patch XML) and `scripts/patches.py`.
- **Edits at prepare time.** `scripts/emevd.py` (EMEVD64: append events, insert instructions),
  `scripts/esd.py` (talk ESD parse and write, byte-identical on all 271 talk scripts) and
  `scripts/fromsoft.py` (DCX/DFLT and BND4). Only built-in features use them: lamps, Rebirth,
  mirror, two invaders.
- **Engine hooks for the port itself.** `src/engine_hooks.h`:
  - Hook types: `bb_hook_entry`, `bb_hook_mid` (all registers), `bb_hook_call`,
    `bb_hook_skip` and `bb_hook_store`.
  - Memory: `bb_patch` (checks the expected bytes), `bb_read` (safe on unmapped addresses).
  - `bb_on_main_frame`, a main-thread tick with limited slots.
  - All hooks are installed in `bb_engine_install()` (`src/engine_features.c`) and then sealed
    by `bb_hooks_seal()` (`src/probe.c`).
- **No plugin or scripting loader.** The roadmap lists "data-field mods and gameplay plugins" as
  wanted but not scheduled.

## Prior art

- **bbhost** (droogie, GPL-3.0-or-later): a separate PC runtime for the same 1.09 eboot.
  - Mods: file overlay, params edited by field name in TOML (`<mods>/params/<Table>.toml`),
    byte patches as TOML manifests.
  - **Native plugin API** (`include/bbhost_plugin.h`, API v12, `docs/plugins.md`). Lifecycle:
    `bb_plugin_info`, `bb_plugin_options`, `bb_plugin_init`, `bb_plugin_image`.
  - API calls:
    - Memory and hooks: `read`, `write`, `patch`, `hook`, `hook2`, `call_guest`, `symbol`,
      `register_hle`.
    - Frame and world events: `on_frame`, `on_world_load`, `loading_screen`.
    - Params: `param_row`, `param_get`, `param_set`.
    - Event flags and Lua: `event_flag_get`, `event_flag_set`, `on_event_flag`, `lua_event`.
    - Player and characters: `player_stat_get`, `player_stat_set`, `player_position`,
      `player_warp`, `lamp_warp`, `chr_list`, `sp_effect_apply`, `sp_effect_has`.
    - Input and camera: `input`, `input_take`, `on_camera`.
    - Text and UI: `replace_text`, `show_message`, `show_text`.
  - C++ SDK: `bbhost/sdk.hpp`, `params.hpp` and engine layouts.
  - Shipped plugins: a randomizer, a boss rush, mutators and a debug menu.
  - yharnam-pc already borrows its hook approach and its ESD code.
- **Soulstruct** (grimrukh): Python toolkit that converts EMEVD and ESD to Python and back. It
  supports Bloodborne. yharnam-pc's Hunter's Atlas generator already uses it.
- **DarkScript3** (EMEVD as JS), **ESDLang**, **Smithbox** (params and maps), **WitchyBND**:
  offline file tools listed on soulsmodding.com. Their output loads as file mods.
- **shadPS4 tools** (Mod Enabler, Vortex, BBLauncher): for the emulator, not this port.

## Sources

- https://github.com/Jgamrot/yharnam-pc (README, docs/ROADMAP.md, src/)
- https://github.com/droogie/bbhost (README.md, docs/plugins.md, docs/modding.md)
- https://github.com/grimrukh/soulstruct
- https://soulsmodding.com/

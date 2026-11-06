# Workplan: plugins and scripting for Bloodborne PC (yharnam-pc)

**Goal.** Let mods run their own code while the game plays. Today mods can only replace files and
apply byte patches. Two layers will be added to the yharnam-pc port:

1. **Native plugins.** DLLs in a `plugins` folder. Each one gets an API for memory, hooks, the
   per-frame tick, event flags, params and the player.
2. **Lua scripts.** Built on the plugin layer, so modders can write `.lua` files instead of
   compiling C.

The API mirrors droogie's bbhost plugin API where it can (names, lifecycle, semantics), so one
plugin can target both runtimes. Background and sources: [RESEARCH.md](RESEARCH.md).

**Baseline.** yharnam-pc 2.3.2 (`port/` submodule, commit `91f63da`, 2026-10-29), CUSA03173 1.09.

---

## Phase 0: Environment and baseline build

- [x] Install MSYS2 (`winget install MSYS2.MSYS2`) and the CLANG64 packages listed in
      `port/packaging/windows/README.md`. Done: clang 23.1.4, cmake 4.4.5, ninja 1.13.2.
- [x] Fork `Jgamrot/yharnam-pc` on GitHub. Point the `port` submodule at the fork and keep
      `upstream` as a second remote. All port work goes on a `plugins` branch.
- [x] `bash build.sh` in the CLANG64 shell builds `out/bb-probe.exe`.
- [x] Run the build against `C:\games\Bloodborne\game` with
      `BB_GAME_DIR=C:/games/Bloodborne/game python run.py`. Use a **separate saves folder**,
      not the installed release's `user\`.
- [x] Run `run.py` with **python.org Python**, not MSYS2's. MSYS2's Python uses `/` as its main
      separator, so `scripts/mods.py`'s `LINKED` lookups miss and report a false "File/directory
      conflict". Upstream fix to send: normalise the keys with `os.path.normpath`.
- [x] Note the startup order in `src/probe.c`: segments mapped, patches applied,
      `bb_hooks_init`, `bb_engine_install`, `bb_hooks_seal`, game start.

**Test baseline (port b4ae1ae, Windows, 2026-11-04)**: `bash tools/test-port.sh`. It builds and runs
every CMake `*-test` target, then the Python suite, and compares both with
`tools/test-baseline.txt`. That file lists the floors, the reason for each skip, and every known
failure by full test id. The run fails on a new failure, and also when fewer tests run or pass, or
more are skipped. A broken build or a missing Python therefore can't look like a clean run.
- **C/C++:** 26 of 33 targets pass.
  - 4 don't link on Windows (undefined runtime symbols, `setenv`).
  - 3 need a trophy data file as their argument.
- **Python:** 394 run, 367 pass, 16 skipped, 11 known failures.
  - The Qt-only launcher modules, Linux packaging, Windows symlinks and certificates.
  - `test_probe`'s POSIX signal test.
- **The 16 skips:**
  - 14 tests need `build.sh --test` executables, which have no Windows build.
  - 1 is CPU-dependent (`test_redzone`).
  - 1 needs Developer Mode for symlinks.
- **Requirement:** python.org Python with `capstone`, `keystone-engine` and `numpy`.

**Done (2026-11-04):** our own build reaches the title screen and plays well (RTX 4090, 91 engine hooks installed).

## Phase 1: Plugin loader (in the fork): done (port 54808d0, cd2a2bf; tool: the hello example)

Plugins load in `bb_engine_install()` (`src/engine_features.c`), after the built-in features
and before `bb_main_frame_install()`. That is the only window before `bb_hooks_seal()` locks
hooking. The guide is `port/docs/PLUGINS.md`.

- [x] `src/plugin_host.c/.h`: `*.dll` in `<data>/plugins`, then `<user>/plugins` (a later folder
      replaces a file of the same name; `BB_PLUGIN_DIRS` overrides both). Each is loaded with
      `LoadLibraryExW` and the altered search path, so a plugin's own folder is searched first
      for its DLLs.
- [x] Lifecycle, as in bbhost:
  - [x] `bb_plugin_info` (optional): name, version, flags. A launcher reading it without
        starting the plugin is Phase 6.
  - [x] `bb_plugin_init`, then `bb_plugin_image`. A nonzero return unloads the plugin together
        with its callbacks. The image functions refuse during init.
  - [x] `register_hle` returns 1 and logs it (decision 2).
- [x] API versioning: bbhost's API 16 table (`version`, `size`). A plugin built for a newer API
      is left out. Every field is set; the missing ones return their failure values and log
      once.
- [x] Off switches:
  - [x] `BB_PLUGINS=0` loads none.
  - [x] Per plugin, in `<user>\plugins.ini`, `[plugins] <name>=0/1`, with opt-in plugins off
        until turned on. This replaces the earlier `bbport.ini` idea: it is bbhost's shape, and
        it leaves the launcher's own file alone (port-54808d0-N1).
  - [x] `BB_ENGINE_HOOKS=0` disables plugins too (`bb_engine_install` never runs).
- [x] **Frame multiplexer:** the host takes one main-frame slot only when a plugin is loaded, and
      runs every `on_frame` callback from it.
- [x] Log lines (`Plugins:` / `Plugin <name>:`) for what loaded and what was left out, and why.
- [x] Online play (`BB_ONLINE=1` without `BB_COOP=1`, the cheats' rule) leaves out plugins
      flagged gameplay.
- [x] Memory functions: `guest_addr`, `read`, `write` (never into code), and `patch` (checks
      expected bytes; idempotent).
- [x] Example `plugins/examples/hello`, built by `tools/build-plugins.sh` (and by the test runner).
      It logs time and frame rate every `interval` seconds, set in plugins.ini. The workplan
      first said "every 300 frames"; time is the better example, because the frame rate varies.

**Done:**
- **Unit test:** `plugin-host-test` makes 101 checks with ten test plugins. The reviewer's
  mutation probe (21 mutations, including the data-into-code write straddle added for
  port-cd2a2bf-N1) kills all 21: reviewer, port-da5ed9c.
- **In game (1.09):** hello loads, reads its option and ticks (5 s, 564 frames, and so on);
  `BB_PLUGINS=0` and `plugins.ini` off leave it out (reviewer, port-cd2a2bf); engine hooks
  stay at 91.
- **Broken plugins:** missing exports, a too-new API, bad or duplicate names and failing phases
  are left out with a log line (unit test).
- **Not tried:** online play in game, and a DLL that fails `LoadLibrary`.

## Phase 2: Core API v1: done (port 1840f95, aedd07c, de208a0)

The plan was thin wrappers over `src/engine_hooks.h`. What landed is bbhost's own API shape,
with a stub and chains of this port's (`src/plugin_hooks.c`). Addresses are Binary Ninja
addresses (base `0x400000`).

- [x] `log`, `config` (`plugins.ini`), `plugin_dir`, `data_dir`, `mods_dir` (Phase 1).
- [x] Memory: `read` (safe on unmapped addresses), `write` (never into code), `patch` (expected
      bytes checked; idempotent), `guest_addr` (Phase 1).
- [x] Hooks: bbhost's `hook` (v1 frame) and `hook2` (`BbHookCtx`: rewrite arguments and xmm,
      skip with a value), not the planned `hook_mid`/`hook_call`, which bbhost's API doesn't
      have.
  - [x] Only during `bb_plugin_image`; earlier or after the seal they return 2.
  - [x] Several plugins on one function chain in load order.
  - [x] Refused over any bytes another hook or patch rewrote: the port's own and other plugins'.
  - [x] The stub's copy of a guest TCB load (`gs:[0]`) gets the TEB slot displacement. A hook
        is refused when that can't be done.
  - [x] A skip returns the value in `rax` and `xmm0` (bbhost: `rax` only).
- [x] `call_guest`: from the game's main thread, once the game runs (not in `bb_plugin_image`),
      only into code. `al` is set for variadic callees.
- [x] `on_frame(fn, user)` (Phase 1).
- [x] The header is bbhost's `include/bbhost_plugin.h` (API 16), unchanged, not a new
      `bbport_plugin.h`. Same names, signatures and table layout, so bbhost plugins build here.
      Loading their DLLs directly works as far as the table goes. Functions this host lacks
      return their failure values.
- [x] Clang/MinGW only for callbacks the game calls directly (`BB_GUEST_ABI`); documented in
      `port/docs/PLUGINS.md`.

**Done:**
- **Unit test:** `plugin-host-test` makes 150 checks. Its hook scenario has three plugins and
  fake functions covering every rule above. My mutation script kills 31 of 32 mutations; the
  survivor is the host's own seal check, which is equivalent because `plugin_hooks` refuses
  after the seal too.
- **In game (1.09):**
  - `hook2` on SprjFlipper::Update (BN `0x2434770`) counts exactly one call per frame.
  - A second plugin's hook joins the chain. The reviewer saw 300/600 hits on both (port-1840f95).
  - An overlapping hook and an early `call_guest` are refused.
  - A TCB-entry hook's stub load matches the image's (`0x1508`). The reviewer ran 55 such hooks
    (port-de208a0).
- **Example:** `plugins/examples/frame_hook` hooks SprjFlipper::Update, counts calls, and
  (`[frame_hook] skip_intro = 1`) `patch()`es the Skip Intro sites with their original bytes
  checked. It builds in the gate. **Not yet run in game** (tool-c25a97f-F1).
- **Not shown in game:** a real `call_guest`. No side-effect-free game function is known yet;
  Phase 3's accessors will use it.

## Phase 3: Game-state API

The port already reaches most of these internally (`src/rebirth.c` has `EVENT_FLAG_MAN` and
`param_row`; `src/pc_menu.c` has messages and menus). Collect them behind the API.

- [x] Event flags: `event_flag_get` (direct read of the flag store, any thread), `event_flag_set`
      (main thread), and an `on_event_flag(fn)` watcher (a hook on the game's setter; one round
      per frame). Port e230c49, a851c5a.
- [x] Params by table and field name: `param_row`, `param_get`, `param_set`, `param_ids`. Field
      names come from the dump's own PARAMDEF files at each start (`scripts/paramdefs.py`), and
      are used only where the row size matches the live table. Port 2c250a9.
- [ ] Player:
  - [x] `player_stat_get` and `player_stat_set` (e230c49), and `player_position` and
        `player_block`, shown in the world (a87a447).
  - [x] `player_warp` and `lamp_warp`: bbhost's routines (port a46c5c9). Only for a plugin
        flagged opt-in and gameplay (the player turned it on; online leaves it out). In the
        world: a 3 m move in Central Yharnam, and a lamp travel with its full load.
  - [x] Warps for Lua scripts: `bb.warp.to` and `bb.warp.lamp`. The player grants them in
        plugins.ini: `[warps] lua = 1` for the Lua host (the host checks, port fe2789c) and the
        script's own name (bb_lua checks), so a downloaded mod's script can't move the player
        because another was allowed to. Online play refuses them in both places. A second,
        gameplay-flagged Lua host was the other choice. It was dropped because two hosts would
        share the one input reader and the one key taker. Run in the world: a move, then a
        lamp travel.
  - [x] `loading_screen()`: bbhost's reading (port 7383ee0); in Lua as `bb.loading()`. A
        picture while it read 1 shows the loading screen.
  - [x] `on_world_load` (port b148dcb).
- [x] Characters: `chr_list` (main thread), `sp_effect_apply` (a SpEffectParam row, a character
      that `chr_list` returns), `sp_effect_has`. Port a87a447, ebb8113.
- [ ] UI and input:
  - [x] `show_message` and `show_text`, drawn by the overlay (port b148dcb).
  - [x] `replace_text`: bbhost's way (port 128d4b0, 73cba1c): the image's leas and pointers to
        a UTF-16 string name a copy of the new text. In the world the map texture path's _9999
        became _9998 in the files the game opened.
  - [x] `input`: keys, mouse buttons, clicks, motion and wheel, counted apart from the game's
        reads; `playing` needs a player in the world. Port c850d52, 37dbc53. In Lua as
        `bb.input` with hotkeys (`bb.input.on_key`), pressed by `game-world.ps1 -Keys` in the world.
  - [x] `input_take`: keys, mouse buttons, wheel and motion taken from the game while playing
        (`src/input_take.c`, port ad77c97). In Lua as `bb.input.take`, every script's together.
        In the world, taking G kept the gestures menu shut while the hotkey still fired.
  - [ ] Plugin options listed in the guide's Mods tab.
- [x] Each call has a host-side test against fake structures at the real addresses
      (game-state-test, flag-delivery-test, params-test). The in-game checks run through
      `tools/game-world.ps1`, which clicks Continue and restores the save afterwards.

**Done when** an example plugin gives the player a buff each time a chosen event flag is set,
and a second one changes a weapon param row by field name. **Both exist and ran in the world**
(tool repo, `plugins/examples/flag_buff` and `weapon_tweak`):
- `flag_buff`, with `[flag_buff] flag = 2000`: after flag 2000 turned on, it applied SpEffect
  2200 (`sp_effect_apply` = 0), and the player then had it (`sp_effect_has` = 1).
- `weapon_tweak`: EquipParamWeapon row 2000000's `attackBasePhysics` went 92 -> 150 at start,
  and a probe read 150 back in the world.
- Since then: `show_message`/`show_text` (port b148dcb), input and `input_take` (c850d52 to
  ad77c97), and `loading_screen` (7383ee0), the warps (a46c5c9) and `replace_text` (128d4b0). Phase 3's API
  is all in, Lua warps included.

## Phase 4: Lua scripting (`bb_lua.dll`)

- [x] Embed Lua 5.4 as a regular plugin (`plugins/lua`, `bb_lua.dll`; Lua 5.4.7 vendored in
      `third_party/`, checksum-verified), so the port itself stays C and Lua stays optional.
      Kept vendored (tool-711f289-Q1; the user left the call to us): builds need no download,
      the version is pinned, and the one patch (lstrlib.c's tick) is in the tree.
- [x] Scripts come from `<user>\plugins\lua\*.lua` and from `mods\<mod>\scripts\*.lua` (named
      `<mod>/<file>`, skipped when the mod is disabled in mods.json), so a mod ships its scripts
      with its files.
- [x] Bindings (docs/LUA.md):
  - [x] Hooks: `bb.on_frame` and `bb.on_world_load`.
  - [x] Flags: `bb.flag.get`, `bb.flag.set` and `bb.flag.on`.
  - [x] Params: `bb.param.get/set/ids(table, id, field)`. The planned `bb.param[table][id].field`
        sugar wasn't built: plain calls keep errors visible.
  - [x] Player and effects: `bb.player.stat/position/block`, `bb.effect.apply/has`,
        `bb.chr.list`, `bb.message` and `bb.text`.
  - [x] Keyboard and mouse: `bb.input.down/pressed/playing/mouse`, `bb.input.on_key` hotkeys,
        and `bb.input.take`.
- [x] Raw memory and hooks are left out of Lua: scripts stay safe, and C plugins cover the rest.
- [x] Sandbox, stricter than planned: no `io`, `require`, `load`, `string.dump` or `debug` at all;
      `os` keeps only `clock`, `time` and `date`. A per-call instruction budget and a 64 MiB
      memory cap per script (docs/LUA.md).
- [x] Error isolation: each script has its own Lua state and runs under `pcall`. A script that
      errors, loops or runs out of memory is stopped alone, with a log line and a banner.
- [ ] Hot reload with a key (scripts only; hooks stay sealed). Next.
- [x] Examples (`plugins/lua/examples/`): a flag-triggered buff, a weapon param tweak, and a
      lamp notification. lua-host-test runs lamp_notify.lua from the folder; in the world, a
      lamp flag set through the game showed "Lamp lit: Central Yharnam (1 of 4)".

**Done when** a mod folder holding only `scripts/*.lua` changes gameplay with no DLL and no
rebuild.

## Phase 5: Script edits shipped with mods (EMEVD / ESD)

The port edits event scripts (EMEVD) and talk scripts (ESD) at prepare time (`scripts/emevd.py`,
`scripts/esd.py`), but only for its built-in features. Open this to mods.

- [ ] Edit files are **data only** (TOML or JSON, never code), so installing a mod never runs
      code from it. See decision 3.
- [ ] Event edits (`event/*.emevd.edit.toml`): add or replace events and insert
      `InitializeEvent`, using named instructions (`SetEventFlag(1234, ON)`). Names come from the
      community's Bloodborne instruction tables (to check: DarkScript3's EMEDF for Bloodborne).
- [ ] Talk edits (`script/talk/*.talk.edit.toml`): a few high-level operations only, such as
      "add a menu option to NPC X that runs event Y", not raw state machines.
- [ ] `prepare.py` applies them to the player's own files, after file mods and in mod order.
- [ ] Mods ship **edits, not FromSoftware files**. That avoids redistribution problems and lets
      two mods edit the same file.
- [ ] Converters from DarkScript3 and Soulstruct output to edit files, so authors keep their
      tools.

**Done when** two mods each add an event to the same `m*.emevd.dcx`, and both work in game.

## Phase 6: SDK, launcher and docs

- [ ] Release package: `sdk/` (headers, a CMake toolchain file for clang, example plugins and
      Lua scripts).
- [ ] The launcher's Mods page lists plugins and scripts with toggles and load errors, read from
      `bb_plugin_info`.
- [ ] Modder guide: `docs/plugins.md` and `docs/lua.md`, and the address conventions (Binary Ninja
      addresses, 1.09 only).
- [ ] CI: build the fork and the examples, and run the host-side tests.

## Phase 7: Upstream

- [ ] Open an issue or discussion on `Jgamrot/yharnam-pc` early (after Phase 1). The roadmap
      already lists "gameplay plugins" as wanted.
- [ ] Keep the `plugins` branch rebased on upstream releases, and keep plugin code in its own
      files with small hooks into existing ones, so rebases stay cheap.
- [ ] Submit it as a pull request once the API is stable.
- [ ] Report to upstream (port-37dbc53-N1): `runtime_pad.c` (the pad read) and
      `bbgpu_mouse_stage_position` call `SDL_GetMouseState` off SDL's thread. The plugin input's
      held-buttons snapshot (window.cpp) could serve them too.
- [ ] Before the PR (port-ad77c97-N1): build.sh's Linux link line lacks `input_bindings.c` and
      `mouse_camera.c` (upstream's own gap), and now `input_take.c` too.

---

## Risks and constraints

| Risk | Mitigation |
|---|---|
| Hooks are sealed after startup, so no hooking later | Every hook is requested in `bb_plugin_image`. Documented, and enforced with an error after seal. |
| Addresses only match CUSA03173 1.09 | Every write and hook checks the expected bytes (`code_check.h`) and is refused on a mismatch. |
| Callbacks need the System V calling convention; MSVC can't produce it | The SDK targets clang and MinGW, with a CMake toolchain file. |
| Thread safety: game state touched off the main thread | `call_guest` and the state setters assert the main thread. `on_frame` is the documented safe place. |
| A plugin crashes the game | The loader can't prevent native crashes. Per-plugin off switches and `BB_PLUGINS=0` keep recovery easy, and the log names the last plugin hook entered. |
| Online fairness | Gameplay plugins turn off online, as cheats do. |
| Upstream changes fast (2.x releases weekly) | Keep plugin code in separate files with few, small hooks into existing code. |
| Licensing (bbhost GPL-3, port GPL-2+) | Decided: the whole project is GPL-3.0-or-later. |

## Decisions (all made 2026-11-04)

1. **bbhost compatibility:** match bbhost's API names, signatures and behaviour, so a plugin
   ports with a recompile and no code changes. Loading bbhost's compiled DLLs directly is nice
   to have, not promised.
2. **Early stage:** `bb_plugin_init` is supported. `register_hle` returns "unsupported" for now,
   to be added only if a popular plugin needs it.
3. **Script-edit format:** data-only edit files (TOML or JSON) with named instructions, plus
   converters from DarkScript3 and Soulstruct. No code ships in mods, edits from several mods
   merge into the player's own files, and no FromSoftware files are redistributed.
4. **Upstream or own fork:** our own fork until the API is
   well-tested in real play, then a pull request. An early heads-up issue upstream (Phase 7) is
   still worthwhile.

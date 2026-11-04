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
- **Unit test:** `plugin-host-test` makes 100 checks with ten test plugins, and the reviewer's
  mutation probe kills all its mutations.
- **In game (1.09):** hello loads, reads its option and ticks (5 s, 564 frames, and so on);
  `BB_PLUGINS=0` and `plugins.ini` off leave it out (reviewer, port-cd2a2bf); engine hooks
  stay at 91.
- **Broken plugins:** missing exports, a too-new API, bad or duplicate names and failing phases
  are left out with a log line (unit test).
- **Not tried:** online play in game, and a DLL that fails `LoadLibrary`.

## Phase 2: Core API v1

These are thin wrappers over `src/engine_hooks.h`. Addresses are Binary Ninja addresses (base
`0x400000`), as in both projects.

- [ ] `log`, `config_get` (the plugin's own ini section), `plugin_dir`.
- [ ] Memory and code:
  - [ ] `read` (`bb_read`, safe on unmapped addresses) and `write`.
  - [ ] `patch`, which writes only if the expected bytes are there (`bb_patch`).
  - [ ] `guest_addr`.
- [ ] Hooks:
  - [ ] `hook` is a function-entry hook (`bb_hook_entry`), `hook_mid` a mid-function hook
        (`bb_hook_mid`, with `BbRegs`) and `hook_call` a call-site hook (`bb_hook_call`).
  - [ ] All three are only valid during `bb_plugin_image`. After seal they return an error.
- [ ] `call_guest`: call a game function (main thread only; checked at runtime).
- [ ] `on_frame(fn, user)`.
- [ ] `include/bbport_plugin.h`: a C ABI header with `BB_GUEST_ABI` (sysv_abi) for any callback
      the game calls. Plugins must be built with clang or MinGW; MSVC can't produce those
      callbacks.
- [ ] Match bbhost's `bbhost_plugin.h` (API v12) in names, signatures and behaviour, so a
      plugin moves between the runtimes with a recompile. Loading bbhost's compiled DLLs
      directly is nice to have, not promised. See decision 1.
      **License decided (2026-11-04):** the project is GPL-3.0-or-later, so bbhost code
      (GPL-3.0-or-later) may be reused directly, with attribution in the file headers.

**Done when** an example plugin installs an entry hook on a known function, counts calls, and
patches a byte site with a check of the original bytes.

## Phase 3: Game-state API

The port already reaches most of these internally (`src/rebirth.c` has `EVENT_FLAG_MAN` and
`param_row`; `src/pc_menu.c` has messages and menus). Collect them behind the API.

- [ ] Event flags: `event_flag_get`, `event_flag_set` and an `on_event_flag(id, fn)` watcher.
- [ ] Params by table and field name: `param_row`, `param_get`, `param_set`, `param_ids`. Field
      names and offsets come from the dump's own `paramdef/` at prepare time, not shipped data.
- [ ] Player:
  - [ ] `player_stat_get` and `player_stat_set`.
  - [ ] `player_position`, `player_warp` and `lamp_warp`.
  - [ ] `loading_screen()`, `on_world_load`.
- [ ] Characters: `chr_list`, `sp_effect_apply`, `sp_effect_has`.
- [ ] UI and input:
  - [ ] `show_message`, `show_text` and `replace_text`.
  - [ ] `input` and `input_take`.
  - [ ] Plugin options listed in the guide's Mods tab.
- [ ] Each call is a host-side test with recorded memory where possible (`port/tests/` has the
      harness), plus a manual check in game.

**Done when** an example plugin gives the player a buff each time a chosen event flag is set,
and a second one changes a weapon param row by field name.

## Phase 4: Lua scripting (`bb_lua.dll`)

- [ ] Embed Lua 5.4 as a regular plugin, so the port itself stays C and Lua stays optional.
- [ ] Scripts come from `plugins/lua/*.lua` and from `mods/<mod>/scripts/*.lua`, so a mod ships
      its scripts with its files.
- [ ] Bindings:
  - [ ] Hooks: `bb.on_frame` and `bb.on_world_load`.
  - [ ] Flags: `bb.flag.get`, `bb.flag.set` and `bb.flag.on`.
  - [ ] Params: `bb.param[table][id].field`.
  - [ ] Player and effects: `bb.player.*`, `bb.effect.apply` and `bb.message`.
- [ ] Raw memory and hooks are left out of Lua at first. Scripts stay safe, and C plugins cover
      the rest.
- [ ] Sandbox: no `os.execute` and no `io` outside the mod's own folder. `require` reads only
      from the mod folder.
- [ ] Error isolation: each script runs under `pcall`. If a script errors, it alone is turned
      off and the error is logged and shown in the guide.
- [ ] Hot reload from the guide, or with a key (scripts only; hooks stay sealed).
- [ ] Examples: a flag-triggered buff, a "New Game+ scaling" param tweak, and a lamp
      notification.

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

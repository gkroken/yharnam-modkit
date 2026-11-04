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

- [ ] Install MSYS2 (`winget install MSYS2.MSYS2`) and the CLANG64 packages listed in
      `port/packaging/windows/README.md`. MSYS2 is not installed yet.
- [ ] Fork `Jgamrot/yharnam-pc` on GitHub. Point the `port` submodule at the fork and keep
      `upstream` as a second remote. All port work goes on a `plugins` branch.
- [ ] `bash build.sh` in the CLANG64 shell builds `out/bb-probe.exe`.
- [ ] Run the build against `C:\games\Bloodborne\game` with
      `BB_GAME_DIR=C:/games/Bloodborne/game python run.py`. Use a **separate saves folder**,
      not the installed release's `user\`.
- [ ] Note the startup order in `src/probe.c`: segments mapped, patches applied,
      `bb_hooks_init`, `bb_engine_install`, `bb_hooks_seal`, game start.

**Done when** our own build reaches the title screen and plays like the installed 1.9.x release.

## Phase 1: Plugin loader (in the fork)

Load plugins in `bb_engine_install()` (`src/engine_features.c`), after the built-in features and
before `bb_main_frame_install()`. That is the only window before `bb_hooks_seal()` locks hooking.

- [ ] `src/plugin_host.c/.h`: find `*.dll` in `<exe dir>/plugins`, then
      `<saves>/plugins` (a later folder overrides a file of the same name). Load each with
      `LoadLibraryW`.
- [ ] Lifecycle, as in bbhost:
  - [ ] `bb_plugin_info` (optional data): name, version, flags. The launcher can read it without
        starting the plugin.
  - [ ] `bb_plugin_image(const BbHostApi*)`: the image is loaded and patched, and hooks are
        still open. A nonzero return unloads the plugin.
  - [ ] `bb_plugin_init`: bbhost calls this before the eboot loads, for `register_hle`. Decide
        whether to support it. HLE in this port lives in `runtime_*.c`, so the likely answer is
        "accepted, no HLE".
- [ ] API versioning: `api->version` plus a struct size, so new fields can be added without
      breaking old plugins.
- [ ] Off switches:
  - [ ] `BB_PLUGINS=0` loads none.
  - [ ] Enable/disable per plugin in `bbport.ini` (`plugin_<name>=0`).
  - [ ] `BB_ENGINE_HOOKS=0` also disables plugins.
- [ ] **Frame multiplexer.** `bb_on_main_frame` has a fixed number of slots
      (`bb_main_frame_available`). The plugin host takes one slot and dispatches it to every
      plugin's `on_frame`.
- [ ] Log what loaded and what was refused (and why) to `user\last_run.log`.
- [ ] Online play: turn off plugins flagged `gameplay` while online, the same way cheats are
      turned off.
- [ ] Example `plugins/examples/hello` logs a line every 300 frames.

**Done when** the hello plugin loads, ticks and unloads cleanly, `BB_PLUGINS=0` skips it, and
a broken DLL (missing exports, too-new API) is refused with a log line instead of crashing.

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
- [ ] **Compatibility decision:** compare against bbhost's `bbhost_plugin.h` (API v12). Copy
      only the names and signatures, or support bbhost plugins as they are? bbhost is
      GPL-3.0-or-later and yharnam-pc is GPL-2.0-or-later. Combining them is allowed, but the
      result is GPL-3. Record the decision here.

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

- [ ] A mod can include `event/*.emevd.edit.py` or a declarative `.toml`: "add event N with these
      instructions", "insert InitializeEvent".
- [ ] The talk equivalent goes in `script/talk/`.
- [ ] `prepare.py` applies them to the player's own files, after file mods and in mod order.
- [ ] Mods ship **edits, not FromSoftware files**. That avoids redistribution problems and lets
      two mods edit the same file.
- [ ] Optional: import from Soulstruct or DarkScript3 sources, for authors who already use
      those tools.

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
| Licensing (bbhost GPL-3, port GPL-2+) | Decided in Phase 2 and recorded here. |

## Open decisions

1. Is bbhost API compatibility an aim (plugins run unchanged on both), or only a style guide?
2. Will we support `bb_plugin_init` and HLE replacement, or only the image stage?
3. Format for mod-shipped EMEVD/ESD edits: Python edit scripts, a declarative TOML, or Soulstruct
   sources?
4. Upstream (PR into yharnam-pc) or maintain our own fork long-term?

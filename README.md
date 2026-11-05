# Bloodborne PC: plugins and scripting

Adds a native plugin API and Lua scripting to the
[yharnam-pc](https://github.com/Jgamrot/yharnam-pc) Bloodborne port, so mods can run code
while the game plays, not only replace files.

- [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md): **start here.** Build, a first Lua script, a first plugin.
- [docs/LUA.md](docs/LUA.md): every Lua call.
- [docs/WORKPLAN.md](docs/WORKPLAN.md): phases, tasks, risks and open decisions.
- [docs/RESEARCH.md](docs/RESEARCH.md): what exists today and the prior art.

## Layout

| Path | What |
|---|---|
| `port/` | yharnam-pc submodule: our fork `gkroken/yharnam-pc`, branch `plugins` (upstream 2.3.2 plus the plugin host; the guide is `port/docs/PLUGINS.md`) |
| `docs/` | The getting-started guide, the Lua reference, the plan and the research |
| `sdk/` | *(Phase 2)* plugin headers and the clang toolchain file |
| `plugins/examples/` | Example plugins (`hello`, `frame_hook`, `flag_buff`, `weapon_tweak`). `bash tools/build-plugins.sh --install` builds them into `port/plugins/`. |
| `plugins/lua/` | `bb_lua`, the Lua script host (`docs/LUA.md`), and example scripts in `examples/`. |
| `third_party/lua-5.4.7/` | Lua's library sources, unchanged (MIT). |
| `tests/` | The tool's own tests (the Lua host against a fake host), run by `tools/test-port.sh`. |
| `tools/` | `test-port.sh` (the test gate, against `test-baseline.txt`), `build-plugins.sh`, and `game-world.ps1` (in-game checks with a loaded character). |

Clone with `git clone --recursive`, or run `git submodule update --init --recursive`.

**License:** GPL-3.0-or-later ([LICENSE](LICENSE)). This is a non-commercial project. Copyleft
keeps every fork and plugin open source.

Local install used for testing: `C:\games\Bloodborne\bbport-windows` (release) and
`C:\games\Bloodborne\game` (CUSA03173 1.09 dump). No game files go in this repository.

# Bloodborne PC: plugins and scripting

Adds a native plugin API and Lua scripting to the
[yharnam-pc](https://github.com/Jgamrot/yharnam-pc) Bloodborne port, so mods can run code
while the game plays, not only replace files.

- [docs/WORKPLAN.md](docs/WORKPLAN.md): phases, tasks, risks and open decisions.
- [docs/RESEARCH.md](docs/RESEARCH.md): what exists today and the prior art.

## Layout

| Path | What |
|---|---|
| `port/` | yharnam-pc submodule (upstream 2.3.2 for now; to be switched to our fork, branch `plugins`) |
| `docs/` | Plan and research |
| `sdk/` | *(Phase 2)* plugin headers and the clang toolchain file |
| `plugins/` | *(Phase 1+)* example plugins and the Lua host (`bb_lua`) |

Clone with `git clone --recursive`, or run `git submodule update --init --recursive`.

Local install used for testing: `C:\games\Bloodborne\bbport-windows` (release) and
`C:\games\Bloodborne\game` (CUSA03173 1.09 dump). No game files go in this repository.

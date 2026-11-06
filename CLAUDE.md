# Bloodborne PC modding tool

Plugins and Lua scripting for the yharnam-pc native port. `port/` is our fork (branch
`plugins`). The coder and reviewer workflow is in `code_review/` (local, not committed): read
`code_review/CODER.md` or `REVIEWER.md` at the start of a session, as your role says.

## Commands (quiet: one line on success, errors only on failure)

| What | Command |
|---|---|
| Test gate (everything) | `bash tools/test-port.sh --quiet` (about 5 min: run it in the background) |
| Build the port | `bash tools/build-port.sh` (game must be closed) |
| Build plugins + Lua host + tool tests | `bash tools/quiet.sh plugins bash tools/build-plugins.sh` |
| Any noisy command | `bash tools/quiet.sh <name> <command...>` (full log in `out/logs/<name>.log`) |
| Wait for a review | `bash tools/wait-review.sh tool\|port [sha]` in the background |

The `in-world-check` and `commit-for-review` skills give the full steps for those two jobs.

## Saving tokens

- Read a log only when a quiet command fails, and then grep it or read just the lines around
  the error, not the whole file.
- Read only the sections you need: grep for a heading or symbol first, then read with an offset
  and limit. Big files: `port/docs/*` (changelogs, research), `tests/lua_host_test.c`,
  `plugins/lua/bb_lua.c`, `port/src/plugin_host.c`.
- Don't list or read `port/out/`, `port/user/`, `third_party/` or `port/bbport-offsets-*`.
- Write multi-line edit scripts with the Write tool, not a heredoc (backslashes get mangled).
- Use a subagent (Explore) for wide searches whose file dumps you don't need.

## Hard rules

- No game bytes, files, saves or keys in the repository.
- Never push or do anything outward-facing without the user's OK.
- Never amend or force-push. Fix forward, citing finding IDs.

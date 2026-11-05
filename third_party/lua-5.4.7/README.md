# Lua 5.4.7

The Lua interpreter's library sources, unchanged, for the `bb_lua` plugin (`plugins/lua`).

- **From:** https://www.lua.org/ftp/lua-5.4.7.tar.gz (2024-06-13).
- **SHA-256:** `9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30`, as listed on
  https://www.lua.org/ftp/ and checked on download.
- **Kept:** `src/*.c` and `src/*.h`, without `lua.c` and `luac.c` (the standalone interpreter
  and compiler, which a plugin doesn't need).
- **One change, marked `bb_lua`:** `src/lstrlib.c` counts the pattern matcher's steps and
  calls `lua_pattern_tick` every 4096 of them, which `plugins/lua/bb_lua.c` defines. Patterns
  run in C, where Lua's instruction-count hook can't reach, so without it one backtracking
  pattern could run forever on the game's main thread (tool-711f289-F1). Everything else is
  as lua.org ships it.
- **License:** MIT (`LICENSE`, the notice at the end of `lua.h`). It is compatible with this
  project's GPL-3.0-or-later.

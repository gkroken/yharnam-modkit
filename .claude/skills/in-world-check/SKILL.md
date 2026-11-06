---
name: in-world-check
description: Run the real game with a plugin or Lua script loaded, get to the world, press keys, take a screenshot and read the plugin log lines. Use for any check that needs the game itself, not a unit test.
---

# In-world check

1. **Rebuild first.** A stale exe or DLL has wasted a run before.
   - The port: `bash tools/build-port.sh`.
   - The plugins: `bash tools/quiet.sh plugins bash tools/build-plugins.sh`.
2. **Write what the run needs into the scratchpad,** not into `port/`: Lua scripts, and a
   `plugins.ini` for settings such as `[warps]` grants or `[lua] reload_key`. The runner puts them
   in place and takes them away again.
3. **Run the check,** from PowerShell or Bash. It backs up the save and puts it and every file
   back afterwards:
   ```
   powershell -ExecutionPolicy Bypass -File tools/game-world.ps1 [-Plugins x.dll,y.dll] [-Scripts a.lua,b.lua] `
       [-Ini my-plugins.ini] [-Seconds 40] [-Keys F8,G] [-KeyHoldMs 120] [-Shot code_review/evidence/<name>.png] [-ShotAfter 2]
   ```
   - `bb_lua.dll` is added from `out/plugins/` when no `-Plugins` entry is one: the runner's own
     Lua script logs `IN_WORLD 0x<block>` when the world loads, which is how it knows.
   - The output is the plugin lines from the log. The full log is `$env:TEMP\bb-game-world.log`; grep it rather than reading all of it.
   - "world NOT reached" is sometimes flaky. Retry once before debugging.
   - Look at screenshots with Read only when the log lines can't answer the question.
4. **Online gating:** use `BB_ONLINE=1` to check refusals, and `BB_COOP=1` for co-op.
5. **Check it cleaned up,** even when the run fails: `git -C port status --short` shows only the
   `fsr-vulkan` line, and there is no `port/plugins` folder.
6. **Report it in the commit message:** what you checked in game, and the log line or the screenshot path. Evidence goes in `code_review/evidence/`.

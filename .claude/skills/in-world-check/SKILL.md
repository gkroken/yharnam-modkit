---
name: in-world-check
description: Run the real game with a plugin or Lua script loaded, get to the world, press keys, take a screenshot and read the plugin log lines. Use for any check that needs the game itself, not a unit test.
---

# In-world check

1. **Rebuild first.** A stale exe or DLL has wasted a run before.
   - The port: `bash tools/build-port.sh`.
   - The plugins: `bash tools/quiet.sh plugins bash tools/build-plugins.sh`.
2. **Set up only what the run needs:**
   - Lua scripts go in `port/user/plugins/lua/<name>.lua`.
   - Settings go in `port/user/plugins.ini`, for example `[warps]` grants or `[lua] reload_key`.
   - Note every file you create; step 5 removes them.
3. **Run the check,** from PowerShell or Bash. It backs up the save and puts it back afterwards:
   ```
   powershell -ExecutionPolicy Bypass -File tools/game-world.ps1 -Plugins out/plugins/bb_lua.dll[,x.dll] `
       [-Seconds 40] [-Keys F8,G] [-KeyHoldMs 120] [-Shot code_review/evidence/<name>.png] [-ShotAfter 2]
   ```
   - The output is the plugin lines from the log. The full log is `$env:TEMP\bb-game-world.log`; grep it rather than reading all of it.
   - The world is reached when a test plugin prints a line containing `IN_WORLD` (the C test plugins do).
   - "world NOT reached" is sometimes flaky. Retry once before debugging.
   - Look at screenshots with Read only when the log lines can't answer the question.
4. **Online gating:** use `BB_ONLINE=1` to check refusals, and `BB_COOP=1` for co-op.
5. **Clean up every time,** even when the run fails:
   - Remove the temporary scripts in `port/user/plugins/lua/`.
   - Remove `port/user/plugins.ini`.
   - Run `git -C port status --short` to check. It should show only the `fsr-vulkan` line.
6. **Report it in the commit message:** what you checked in game, and the log line or the screenshot path. Evidence goes in `code_review/evidence/`.

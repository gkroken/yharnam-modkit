# Instructions for coding agents

You are helping someone make a mod that runs code in Bloodborne on the yharnam-pc PC port, with
this SDK. Read this first; `docs/GUIDE.md` is the full guide.

## What to read before writing code

- Lua (the default): `docs/LUA.md`, every `bb.*` call and what a script can't do.
- C (only for hooks, calls into the game or raw memory): `include/bbhost_plugin.h` (every call,
  documented) and `docs/PLUGINS.md` (what this host provides and its rules).
- Examples that work: `examples/lua/`, `examples/c/`, and `template/` for a new plugin.

Prefer Lua. It needs no build, a mistake stops only that script, and it reloads without a restart.

## The loop

1. **Find the names you need** before guessing them:
   - param tables and fields: grep `<install>/out/paramdefs.txt` (written at each game start);
   - event flags: log them while the player does the thing in game
     (`bb.flag.on(function(id, on) bb.log(id, on) end)`), or ask the person;
   - the map: `bb.player.block()`.
2. **Write the script or plugin.** Log what it does with `bb.log`, so the log can prove it.
3. **Run it in the world:**
   ```
   powershell -ExecutionPolicy Bypass -File tools/game-world.ps1 -Install <port tree> `
       -Scripts my.lua [-Plugins my.dll] [-Ini my-plugins.ini] [-Keys F6,G] [-Shot shot.png]
   ```
   `-Mods mods/<mod>` runs whole mod folders instead (their files, `scripts/` and `edits/`); a
   mod's event edits (`edits/event/*.emevd.edit.toml`, `docs/GUIDE.md` section 6) report as
   `Script edits:` lines.
   It backs up the save, loads the character, presses the keys (F1..F12, A..Z), prints every
   `Plugin`/`Plugins` log line and puts everything back. It needs the game's window in front and
   moves the mouse, so tell the person before you start it.
4. **Read the printed lines.** Your script's lines are `Plugin lua: <file name>: ...`.
   `stopped: <why>` is your error, with the line number. `Plugins: ... left out, <why>` says why a
   plugin didn't load. `world NOT reached` is sometimes a fluke: run once more before debugging.
5. **Only call it working when a log line (or the picture) shows it working in the world.** A
   script that loads isn't yet a script that does its job.

If the person runs the game themselves, `[lua] reload_key = f8` in `<user>/plugins.ini` reloads
every script from disk without a restart; ask them to press it and paste the log.

## Rules

- **Never put game files or bytes copied from the game** in a mod or a repository: no `eboot`,
  `.dcx`, saves or keys. Recognise game code by its length and a hash.
- **Every write into the game is checked:** pass the bytes you expect to `patch` and the function's
  first bytes to `hook`, read from memory with `read()` (not from `eboot.elf`). A mismatch is
  refused; never work around a refusal by passing NULL without knowing why it was refused.
- **Name every address** you use: a constant, what it is, and how it's known. Addresses are Binary
  Ninja's for the 1.09 eboot at base `0x400000`.
- **Change the game only from `on_frame`** (or Lua, which runs there) and flag a plugin that
  changes the game `BB_PLUGIN_GAMEPLAY`, so online play leaves it out.
- **Don't touch the person's saves** beyond what `game-world.ps1` backs up and restores, and don't
  play online with a mod you're testing.
- **Ask the person before** anything outside their machine: publishing a mod, uploading logs or
  pictures, opening issues.

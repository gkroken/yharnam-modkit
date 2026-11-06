# SPDX-License-Identifier: GPL-3.0-or-later
# Starts the game from a source tree of the plugins fork with the plugins, Lua scripts and
# plugins.ini given, clicks Continue at the title, waits in the world, presses keys, takes a
# picture, closes the game, and puts back everything it changed (the save first of all). Prints
# the log's plugin lines. For checks that need a loaded character, by a person or a coding agent.
#
#   powershell -ExecutionPolicy Bypass -File tools/game-world.ps1 [-Install <port tree>]
#       [-Plugins a.dll,b.dll] [-Scripts my.lua,other.lua] [-Ini my-plugins.ini]
#       [-Seconds 40] [-Keys F6,G] [-KeyHoldMs 120] [-Shot shot.png] [-ShotAfter 2]
#
# - -Install: the port source tree to run (its run.py, out\, user\). Default: the port\ folder
#   beside this script's folder (this repository's submodule). Released packages of yharnam-pc
#   have no plugin host yet, so they can't be used.
# - -Plugins: DLLs copied into <install>\plugins for the run. -Scripts: Lua files copied into
#   <user>\plugins\lua. -Ini: a file used as <user>\plugins.ini. A file of the same name already
#   there is set aside and put back afterwards.
# - "In the world" is a log line containing IN_WORLD. The runner adds a Lua script of its own that
#   logs it when the world loads, so bb_lua.dll must load: one in -Plugins or <install>\plugins,
#   else -LuaHost (default: ..\bin\bb_lua.dll beside this script, as in the SDK, else this
#   repository's out\plugins\bb_lua.dll). A test plugin may print IN_WORLD itself, too.
# - Needs the install's own save (<user>\savedata, with a character), and the game's window on
#   the primary display: it moves the mouse and clicks Continue.
# - -Keys: once in the world, each key (F1..F12, A..Z) is pressed and released in turn, a second
#   apart, but only while the game's window is in front (never into anyone else's window). Each is
#   held -KeyHoldMs (120); 0 is a tap shorter than a frame.
# - -Shot <file.png>: a picture of the game's window -ShotAfter seconds (2) after the keys (or after
#   reaching the world), taken from the screen, so only while the game is in front.
# - The save is copied first and put back afterwards, so a run leaves it as it was.
# - The dump: BB_GAME_DIR, else C:/games/Bloodborne/game. Python: -Python, else the per-user
#   python.org 3.13. <user> is BB_USER_DIR, else <install>\user.
param([string]$Install = '', [string[]]$Plugins = @(), [string[]]$Scripts = @(), [string]$Ini = '',
      [string]$LuaHost = '', [int]$Seconds = 40, [string[]]$Keys = @(), [int]$KeyHoldMs = 120,
      [string]$Shot = '', [int]$ShotAfter = 2, [string]$Log = "$env:TEMP\bb-game-world.log",
      [string]$Python = "$env:LOCALAPPDATA\Programs\Python\Python313\python.exe")
$ErrorActionPreference = 'Stop'
$root = if ($Install) { (Resolve-Path $Install).Path } else { Join-Path (Split-Path -Parent $PSScriptRoot) 'port' }
if (-not (Test-Path "$root\run.py")) { throw "$root is not a port source tree (no run.py): pass -Install" }
$user = if ($env:BB_USER_DIR) { $env:BB_USER_DIR } else { "$root\user" }
# powershell -File passes a list as one comma-joined string: split it.
$Plugins = @($Plugins | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Scripts = @($Scripts | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Keys = @($Keys | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
foreach ($k in $Keys) { if ($k -notmatch '^(F([1-9]|1[0-2])|[A-Za-z])$') { throw "-Keys takes F1..F12 and A..Z, not $k" } }
# Everything is checked before anything is touched.
foreach ($p in $Plugins + $Scripts) { if (-not (Test-Path $p)) { throw "no file at $p" } }
if ($Ini -and -not (Test-Path $Ini)) { throw "no file at $Ini" }
if (-not (Test-Path "$user\savedata")) { throw "no save at $user\savedata: start the game once and make a character" }
$hasLua = ($Plugins | Where-Object { (Split-Path $_ -Leaf) -eq 'bb_lua.dll' }) -or (Test-Path "$root\plugins\bb_lua.dll")
if (-not $hasLua) {
    $candidates = @($LuaHost, (Join-Path $PSScriptRoot '..\bin\bb_lua.dll'), (Join-Path $PSScriptRoot '..\out\plugins\bb_lua.dll'))
    $found = $candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    if (-not $found) { throw 'no bb_lua.dll to run the world marker with: pass -LuaHost or put one in -Plugins' }
    $Plugins += (Resolve-Path $found).Path
}
$marker = 'zz_game_world_marker.lua'
$markerSource = Join-Path $env:TEMP "bb-game-world-$PID-$marker"
Set-Content -Path $markerSource -Encoding ascii -Value 'bb.on_world_load(function(block) bb.log(("IN_WORLD 0x%08x"):format(block)) end)'
# What the run puts in place: the file to copy, and where it goes.
$placed = New-Object System.Collections.ArrayList
foreach ($p in $Plugins) { [void]$placed.Add([pscustomobject]@{ From = $p; To = Join-Path "$root\plugins" (Split-Path $p -Leaf) }) }
foreach ($s in $Scripts) { [void]$placed.Add([pscustomobject]@{ From = $s; To = Join-Path "$user\plugins\lua" (Split-Path $s -Leaf) }) }
[void]$placed.Add([pscustomobject]@{ From = $markerSource; To = "$user\plugins\lua\$marker" })
if ($Ini) { [void]$placed.Add([pscustomobject]@{ From = $Ini; To = "$user\plugins.ini" }) }
$backup = Join-Path $env:TEMP "bb-game-world-save-$PID"
$copied = @()
$kept = @()
$madeDirs = @()
$backedUp = $false
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class GW { [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
[DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
[DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, UIntPtr e);
[DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
public struct RECT { public int Left, Top, Right, Bottom; }
public struct POINT { public int X, Y; } }
'@
# Clicks only when the game is the foreground window and the point under the cursor is the game's
# (Windows may refuse to bring a background process's window forward): never anyone else's window.
function Click($handle, $x, $y) {
    if ([GW]::GetForegroundWindow() -ne $handle) { return $false }
    $pt = New-Object GW+POINT; $pt.X = $x; $pt.Y = $y
    if ([GW]::GetAncestor([GW]::WindowFromPoint($pt), 2) -ne $handle) { return $false }  # GA_ROOT
    [GW]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80; [GW]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    return $true
}
# Presses one key (a scancode event, as SDL reads keys) when the game is in front.
function Press($handle, $key) {
    if ([GW]::GetForegroundWindow() -ne $handle) { "key $key not pressed: the game is not in front"; return }
    $vk = if ($key.Length -gt 1) { 0x6F + [int]$key.Substring(1) } else { [int][char]$key.ToUpper() }
    $scan = [byte][GW]::MapVirtualKey($vk, 0)
    [GW]::keybd_event([byte]$vk, $scan, 8, [UIntPtr]::Zero); Start-Sleep -Milliseconds $KeyHoldMs   # KEYEVENTF_SCANCODE
    [GW]::keybd_event([byte]$vk, $scan, 10, [UIntPtr]::Zero)                                 # + KEYUP
    "key $key pressed"
}
$cursor = New-Object GW+POINT; [void][GW]::GetCursorPos([ref]$cursor)
try {
    # Restore only from a backup that finished: a copy failing partway (a locked file, a full disk)
    # throws with part of the save in $backup, which must never replace the real one.
    try {
        Copy-Item -Recurse "$user\savedata" $backup
    } catch {
        Remove-Item -Recurse -Force $backup -ErrorAction SilentlyContinue
        throw "the save could not be backed up ($_); nothing was changed"
    }
    $backedUp = $true
    "save backed up to $backup (it is put back at the end; if this run is cut short, copy it back)"
    foreach ($pair in $placed) {
        $source = $pair.From; $target = $pair.To
        $dir = Split-Path $target
        # Folders the run makes are removed again if they end up empty (never one that was there).
        $missing = @(); $d = $dir
        while ($d -and -not (Test-Path $d)) { $missing = @($d) + $missing; $d = Split-Path $d }
        foreach ($m in $missing) { New-Item -ItemType Directory $m | Out-Null; $madeDirs = @($m) + $madeDirs }
        # A file of the same name already there (the developer's own) is kept aside and put back.
        if (Test-Path $target) { Move-Item $target "$target.game-world-kept"; $kept += $target }
        Copy-Item $source $target
        $copied += $target
    }
    if (-not $env:BB_GAME_DIR) { $env:BB_GAME_DIR = 'C:/games/Bloodborne/game' }
    Start-Process -FilePath $Python -ArgumentList "`"$root\run.py`"" -WorkingDirectory $root -RedirectStandardOutput $Log `
        -RedirectStandardError "$Log.err" -WindowStyle Hidden | Out-Null
    $game = $null
    for ($i = 0; $i -lt 120 -and -not $game; $i++) {
        Start-Sleep 1
        $game = Get-Process bb-probe -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root*" } | Select-Object -First 1
    }
    if (-not $game) { throw 'the game did not start' }
    Start-Sleep 35  # the title, after shader builds and the intro
    # Continue sits at about 50% across and 63% down the window (title menu, any 16:9 size).
    $world = $false
    for ($try = 0; $try -lt 6 -and -not $world; $try++) {
        $game.Refresh()
        [void][GW]::SetForegroundWindow($game.MainWindowHandle); Start-Sleep -Milliseconds 400
        $r = New-Object GW+RECT; [void][GW]::GetWindowRect($game.MainWindowHandle, [ref]$r)
        $x = [int]($r.Left + ($r.Right - $r.Left) * 0.502); $y = [int]($r.Top + ($r.Bottom - $r.Top) * 0.632)
        [void][GW]::SetCursorPos($x, $y); Start-Sleep -Milliseconds 300
        $handle = $game.MainWindowHandle
        if (-not (Click $handle $x $y)) { 'the game is not in front: not clicking, trying again'; Start-Sleep 3; continue }
        Start-Sleep 2
        [void](Click $handle $x $y)
        for ($w = 0; $w -lt 60 -and -not $world; $w++) { Start-Sleep -Milliseconds 500; $world = Select-String -Path $Log -Pattern 'IN_WORLD' -Quiet }
    }
    if ($world) {
        'world reached'
        if ($Keys) { Start-Sleep 3; foreach ($k in $Keys) { Press $handle $k; Start-Sleep 1 } }
        if ($Shot) {
            Start-Sleep $ShotAfter
            if ([GW]::GetForegroundWindow() -ne $handle) { 'no picture: the game is not in front' } else {
                Add-Type -AssemblyName System.Drawing
                $r = New-Object GW+RECT; [void][GW]::GetWindowRect($handle, [ref]$r)
                $bmp = New-Object System.Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
                $g = [System.Drawing.Graphics]::FromImage($bmp)
                $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
                $bmp.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
                "picture: $Shot"
            }
        }
        Start-Sleep $Seconds
    } else { 'world NOT reached' }
} finally {
    [void][GW]::SetCursorPos($cursor.X, $cursor.Y)
    Get-Process bb-probe -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root*" } | Stop-Process -Force
    Start-Sleep 2
    if ($backedUp) {
        # The game may hold a file for a moment after it is killed: retry, and copy back only into
        # a savedata that is really gone (Copy-Item into an existing folder would nest the backup).
        for ($i = 0; $i -lt 10 -and (Test-Path "$user\savedata"); $i++) {
            Remove-Item -Recurse -Force "$user\savedata" -ErrorAction SilentlyContinue
            if (Test-Path "$user\savedata") { Start-Sleep 1 }
        }
        if (Test-Path "$user\savedata") {
            "could NOT put the save back: $user\savedata is still there. The backup is kept at $backup"
        } else {
            Copy-Item -Recurse $backup "$user\savedata"
            Remove-Item -Recurse -Force $backup
        }
    }
    foreach ($t in $copied) { Remove-Item -Force $t -ErrorAction SilentlyContinue }
    foreach ($t in $kept) { Move-Item "$t.game-world-kept" $t }
    foreach ($m in $madeDirs) {  # deepest first
        if (-not (Get-ChildItem $m -Force -ErrorAction SilentlyContinue)) { Remove-Item $m -ErrorAction SilentlyContinue }
    }
    Remove-Item -Force $markerSource -ErrorAction SilentlyContinue
}
Select-String -Path $Log -Pattern '^Plugin|^Plugins|Engine hooks|Fault|exception 0x' | ForEach-Object { $_.Line }

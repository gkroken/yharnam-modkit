# SPDX-License-Identifier: GPL-3.0-or-later
# Starts the source-tree game (port/) with the given plugin DLLs, clicks Continue at the title,
# waits in the world, closes the game, and restores the save it found. Prints the log's plugin
# lines. For in-game checks of the game-state API, which need a loaded character.
#
#   powershell -ExecutionPolicy Bypass -File tools/game-world.ps1 -Plugins a.dll,b.dll [-Seconds 40] [-Keys F6,G]
#
# - "In the world" is a log line containing IN_WORLD, which the test plugin prints once
#   player_block succeeds.
# - Needs the source tree's own save (port\user\savedata, with a character), and the game's
#   window on the primary display: it moves the mouse and clicks Continue.
# - -Keys: once in the world, each key (F1..F12, A..Z) is pressed and released in turn, a second
#   apart, but only while the game's window is in front (never into anyone else's window). Each is
#   held -KeyHoldMs (120); 0 is a tap shorter than a frame.
# - The save is copied first and put back afterwards, so a run leaves it as it was.
# - The dump: BB_GAME_DIR, else C:/games/Bloodborne/game. Python: -Python, else the per-user
#   python.org 3.13.
param([string[]]$Plugins = @(), [int]$Seconds = 40, [string[]]$Keys = @(), [int]$KeyHoldMs = 120, [string]$Log = "$env:TEMP\bb-game-world.log",
      [string]$Python = "$env:LOCALAPPDATA\Programs\Python\Python313\python.exe")
$ErrorActionPreference = 'Stop'
$root = Join-Path (Split-Path -Parent $PSScriptRoot) 'port'
# powershell -File passes a list as one comma-joined string: split it.
$Plugins = @($Plugins | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Keys = @($Keys | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
foreach ($k in $Keys) { if ($k -notmatch '^(F([1-9]|1[0-2])|[A-Za-z])$') { throw "-Keys takes F1..F12 and A..Z, not $k" } }
foreach ($p in $Plugins) { if (-not (Test-Path $p)) { throw "no plugin at $p" } }  # before touching anything
$backup = Join-Path $env:TEMP "bb-game-world-save-$PID"
$copied = @()
$kept = @()
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
        Copy-Item -Recurse "$root\user\savedata" $backup
    } catch {
        Remove-Item -Recurse -Force $backup -ErrorAction SilentlyContinue
        throw "the save could not be backed up ($_); nothing was changed"
    }
    $backedUp = $true
    "save backed up to $backup (it is put back at the end; if this run is cut short, copy it back)"
    New-Item -ItemType Directory -Force "$root\plugins" | Out-Null
    # A plugin of the same name already in port\plugins (the developer's own) is kept aside and put back.
    foreach ($p in $Plugins) {
        $target = Join-Path "$root\plugins" (Split-Path $p -Leaf)
        if (Test-Path $target) { Move-Item $target "$target.game-world-kept"; $kept += $target }
        Copy-Item $p "$root\plugins\"
        $copied += $target
    }
    if (-not $env:BB_GAME_DIR) { $env:BB_GAME_DIR = 'C:/games/Bloodborne/game' }
    Start-Process -FilePath $Python -ArgumentList "$root\run.py" -WorkingDirectory $root -RedirectStandardOutput $Log `
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
        for ($w = 0; $w -lt 15 -and -not $world; $w++) { Start-Sleep 2; $world = Select-String -Path $Log -Pattern 'IN_WORLD' -Quiet }
    }
    if ($world) {
        'world reached'
        if ($Keys) { Start-Sleep 3; foreach ($k in $Keys) { Press $handle $k; Start-Sleep 1 } }
        Start-Sleep $Seconds
    } else { 'world NOT reached' }
} finally {
    [void][GW]::SetCursorPos($cursor.X, $cursor.Y)
    Get-Process bb-probe -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root*" } | Stop-Process -Force
    Start-Sleep 2
    if ($backedUp) {
        # The game may hold a file for a moment after it is killed: retry, and copy back only into
        # a savedata that is really gone (Copy-Item into an existing folder would nest the backup).
        for ($i = 0; $i -lt 10 -and (Test-Path "$root\user\savedata"); $i++) {
            Remove-Item -Recurse -Force "$root\user\savedata" -ErrorAction SilentlyContinue
            if (Test-Path "$root\user\savedata") { Start-Sleep 1 }
        }
        if (Test-Path "$root\user\savedata") {
            "could NOT put the save back: port\user\savedata is still there. The backup is kept at $backup"
        } else {
            Copy-Item -Recurse $backup "$root\user\savedata"
            Remove-Item -Recurse -Force $backup
        }
    }
    foreach ($t in $copied) { Remove-Item -Force $t -ErrorAction SilentlyContinue }
    foreach ($t in $kept) { Move-Item "$t.game-world-kept" $t }
    if (-not (Get-ChildItem "$root\plugins" -ErrorAction SilentlyContinue)) { Remove-Item "$root\plugins" -ErrorAction SilentlyContinue }
}
Select-String -Path $Log -Pattern '^Plugin|^Plugins|Engine hooks|Fault|exception 0x' | ForEach-Object { $_.Line }

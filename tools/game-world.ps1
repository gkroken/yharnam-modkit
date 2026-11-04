# SPDX-License-Identifier: GPL-3.0-or-later
# Starts the source-tree game (port/) with the given plugin DLLs, clicks Continue at the title,
# waits in the world, closes the game, and restores the save it found. Prints the log's plugin
# lines. For in-game checks of the game-state API, which need a loaded character.
#
#   powershell -ExecutionPolicy Bypass -File tools/game-world.ps1 -Plugins a.dll,b.dll [-Seconds 40]
#
# - "In the world" is a log line containing IN_WORLD, which the test plugin prints once
#   player_block succeeds.
# - Needs the source tree's own save (port\user\savedata, with a character), and the game's
#   window on the primary display: it moves the mouse and clicks Continue.
# - The save is copied first and put back afterwards, so a run leaves it as it was.
# - The dump: BB_GAME_DIR, else C:/games/Bloodborne/game. Python: -Python, else the per-user
#   python.org 3.13.
param([string[]]$Plugins = @(), [int]$Seconds = 40, [string]$Log = "$env:TEMP\bb-game-world.log",
      [string]$Python = "$env:LOCALAPPDATA\Programs\Python\Python313\python.exe")
$ErrorActionPreference = 'Stop'
$root = Join-Path (Split-Path -Parent $PSScriptRoot) 'port'
$backup = Join-Path $env:TEMP "bb-game-world-save-$PID"
Copy-Item -Recurse "$root\user\savedata" $backup
New-Item -ItemType Directory -Force "$root\plugins" | Out-Null
foreach ($p in $Plugins) { Copy-Item $p "$root\plugins\" }
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class GW { [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
public struct RECT { public int Left, Top, Right, Bottom; } }
'@
function Click { [GW]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80; [GW]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero) }
try {
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
        Click; Start-Sleep 2; Click
        for ($w = 0; $w -lt 15 -and -not $world; $w++) { Start-Sleep 2; $world = Select-String -Path $Log -Pattern 'IN_WORLD' -Quiet }
    }
    if ($world) { 'world reached'; Start-Sleep $Seconds } else { 'world NOT reached' }
} finally {
    Get-Process bb-probe -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root*" } | Stop-Process -Force
    Start-Sleep 2
    Remove-Item -Recurse -Force "$root\user\savedata"
    Copy-Item -Recurse $backup "$root\user\savedata"
    Remove-Item -Recurse -Force $backup
    foreach ($p in $Plugins) { Remove-Item -Force (Join-Path "$root\plugins" (Split-Path $p -Leaf)) -ErrorAction SilentlyContinue }
    if (-not (Get-ChildItem "$root\plugins" -ErrorAction SilentlyContinue)) { Remove-Item "$root\plugins" -ErrorAction SilentlyContinue }
}
Select-String -Path $Log -Pattern '^Plugin|^Plugins|Engine hooks|Fault|exception 0x' | ForEach-Object { $_.Line }

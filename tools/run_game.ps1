<#
.SYNOPSIS
Launches Fallout: New Vegas with xNVSE and presses it through to the last save.

.DESCRIPTION
Steam's steam://rungameid/22380 is the wrong door: it starts FalloutNVLauncher.exe,
a separate launcher app that then has to launch the game itself. Two extra programs,
two extra failure modes.

This starts nvse_loader.exe instead. It is xNVSE's own entry point, it loads
nvse_1_4.dll plus nvse_steam_loader.dll, and it starts FalloutNV.exe directly - no
Fallout NV Launcher in the middle. VaultCraft needs xNVSE loaded, so this is also
the only supported way to run it.

Synthetic key input goes through the keyboard state, so the game only sees it while
its window is genuinely foreground. A plain AppActivate from a background process
often fails, silently, and the keys then go to whatever does have focus. This forces
foreground properly - attaching to the foreground thread's input queue - and then
checks the result by watching VaultCraft.log for the save actually loading, retrying
the keys if it does not.

.PARAMETER MenuWaitSeconds
How long to wait after the window appears before pressing anything. The window
exists long before it will accept input, and early keys are discarded silently.

.PARAMETER NoKeys
Launch and wait, but do not press anything.
#>
[CmdletBinding()]
param(
    [string] $GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\Fallout New Vegas',
    [int]    $MenuWaitSeconds = 40,
    [int]    $SkipPresses = 6,
    [switch] $NoKeys,
    [switch] $NoVerify
)

$ErrorActionPreference = 'Stop'

$loader = Join-Path $GameDir 'nvse_loader.exe'
$logFile = Join-Path $GameDir 'VaultCraft.log'

if (-not (Test-Path -LiteralPath $loader)) {
    throw "nvse_loader.exe is not in '$GameDir'. It ships with xNVSE, and it is the only entry point that loads NVSE while skipping the Fallout NV Launcher."
}

# Steam is still required even though we launch outside it: the game checks for a
# running client at startup and nvse_steam_loader.dll only satisfies the handshake,
# not the client check.
if (-not (Get-Process steam -ErrorAction SilentlyContinue)) {
    throw 'Steam is not running. Fallout: New Vegas checks for it at startup.'
}

Add-Type -AssemblyName System.Windows.Forms

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Win {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();

    // SetForegroundWindow only works from the thread that owns the current foreground
    // window. Attaching to that thread's input queue for the duration of the call is
    // the standard way to get around it, and without it AppActivate-equivalents fail
    // from a background process and the keys land somewhere else entirely.
    public static bool ForceForeground(IntPtr hwnd) {
        uint fgPid;
        uint fgThread = GetWindowThreadProcessId(GetForegroundWindow(), out fgPid);
        uint me = GetCurrentThreadId();
        if (fgThread != 0 && fgThread != me) {
            AttachThreadInput(me, fgThread, true);
        }
        ShowWindow(hwnd, 9);              // SW_RESTORE - un-minimises if needed
        BringWindowToTop(hwnd);
        bool ok = SetForegroundWindow(hwnd);
        if (fgThread != 0 && fgThread != me) {
            AttachThreadInput(me, fgThread, false);
        }
        return ok;
    }
}
'@

function Get-SaveLoadCount {
    # The game holds this log with _SH_DENYWR, which still permits readers, so it can be
    # tailed while the game runs. Explicit sharing is required or the open fails.
    if (-not (Test-Path -LiteralPath $logFile)) { return 0 }
    try {
        $fs = [IO.File]::Open($logFile, [IO.FileMode]::Open, [IO.FileAccess]::Read,
                              ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        $sr = New-Object IO.StreamReader($fs)
        $text = $sr.ReadToEnd()
        $sr.Close(); $fs.Close()
        return ([regex]::Matches($text, 'save loaded')).Count
    } catch {
        return 0
    }
}

$proc = Get-Process FalloutNV -ErrorAction SilentlyContinue | Select-Object -First 1
if ($proc) {
    Write-Host "Fallout: New Vegas is already running (pid $($proc.Id)); attaching to it."
}
else {
    Write-Host 'Starting nvse_loader.exe ...'
    Start-Process -FilePath $loader -WorkingDirectory $GameDir | Out-Null

    $deadline = (Get-Date).AddMinutes(3)
    while (-not $proc -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        $proc = Get-Process FalloutNV -ErrorAction SilentlyContinue | Select-Object -First 1
    }
    if (-not $proc) {
        throw 'FalloutNV.exe never appeared. Check nvse.log and nvse_steam_loader.log in the game folder.'
    }
    Write-Host "FalloutNV.exe running (pid $($proc.Id))."
}

$deadline = (Get-Date).AddMinutes(2)
while ((Get-Date) -lt $deadline) {
    $proc.Refresh()
    if ($proc.MainWindowHandle -ne 0) { break }
    Start-Sleep -Milliseconds 500
}
if ($proc.MainWindowHandle -eq 0) {
    throw "FalloutNV.exe (pid $($proc.Id)) has no main window yet."
}
Write-Host "Window up. Waiting $MenuWaitSeconds s for the menu to settle."
Start-Sleep -Seconds $MenuWaitSeconds

if ($NoKeys) {
    Write-Host '-NoKeys given; leaving the game at the main menu.'
    return
}

# ESC skips the long loading sequence at the start of a session, which is worth a
# press or three but not a flood: it is also the pause/menu key later on.
for ($i = 0; $i -lt $SkipPresses; $i++) {
    [void][Win]::ForceForeground($proc.MainWindowHandle)
    Start-Sleep -Milliseconds 400
    [System.Windows.Forms.SendKeys]::SendWait('{ESC}')
    Write-Host "  skip press $($i + 1)/$SkipPresses"
    Start-Sleep -Seconds 2
}

$baseline = -1
if (-not $NoVerify) { $baseline = Get-SaveLoadCount }

$attempt = 0
while ($true) {
    $attempt++
    $forced = [Win]::ForceForeground($proc.MainWindowHandle)
    $fg = [Win]::GetForegroundWindow()
    Write-Host ("Attempt {0}: focus forced={1}, foreground now ours={2}" -f $attempt, $forced, ($fg -eq $proc.MainWindowHandle))
    Start-Sleep -Milliseconds 600

    # Up, then Enter, pause, Enter: pick Continue on the main menu, then clear the
    # prompt that appears once the world starts loading.
    [System.Windows.Forms.SendKeys]::SendWait('{UP}')
    Start-Sleep -Milliseconds 1200
    [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
    Start-Sleep -Seconds 2
    [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')

    if ($NoVerify) { break }

    # Watch the plugin's log for the save actually loading. This is the only check
    # that matters: whether the keys were seen is otherwise a guess, and a guess that
    # has already been wrong once.
    $deadline = (Get-Date).AddSeconds(30)
    $ok = $false
    while ((Get-Date) -lt $deadline) {
        if ((Get-SaveLoadCount) -gt $baseline) { $ok = $true; break }
        Start-Sleep -Seconds 1
    }
    if ($ok) {
        Write-Host 'Save loaded - the keys landed.'
        break
    }
    if ($attempt -ge 3) {
        Write-Warning "Save still has not loaded after $attempt attempts. The game is probably up at the main menu; press Continue by hand."
        break
    }
    Write-Host 'No save load seen; refocusing and trying again.'
}
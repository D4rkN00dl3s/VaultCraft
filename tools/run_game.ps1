<#
.SYNOPSIS
Launches Fallout: New Vegas with xNVSE and presses it through to the last save.

.DESCRIPTION
Steam's steam://rungameid/22380 is the wrong door: it starts FalloutNVLauncher.exe,
which is a separate launcher app that then has to launch the game itself. Two extra
programs, two extra failure modes.

This starts nvse_loader.exe instead. It is xNVSE's own entry point, it loads
nvse_1_4.dll plus nvse_steam_loader.dll, and it starts FalloutNV.exe directly - no
Fallout NV Launcher in the middle. VaultCraft needs xNVSE loaded, so this is also
the only supported way to run it.

The game window exists long before it will accept input, so this waits for the main
window handle and then for the menu to settle before sending anything. Sending keys
too early is silently ignored, which looks exactly like the script not working.

.PARAMETER MenuWaitSeconds
How long to wait after the window appears before pressing anything. The default is
generous because being early does nothing while being late only means the keys land
on the already-loaded game.

.PARAMETER NoKeys
Launch and wait, but do not press anything. Useful when the game is already past the
menu, or when debugging.
#>
[CmdletBinding()]
param(
    [string] $GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\Fallout New Vegas',
    [int]    $MenuWaitSeconds = 45,
    [int]    $BetweenKeysMs = 1500,
    [switch] $NoKeys
)

$ErrorActionPreference = 'Stop'

$loader = Join-Path $GameDir 'nvse_loader.exe'
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

$proc = Get-Process FalloutNV -ErrorAction SilentlyContinue | Select-Object -First 1
if ($proc) {
    Write-Host "Fallout: New Vegas is already running (pid $($proc.Id)); attaching to it."
}
else {
    Write-Host 'Starting nvse_loader.exe ...'
    Start-Process -FilePath $loader -WorkingDirectory $GameDir | Out-Null

    # The loader is a stub; FalloutNV.exe is the process that appears.
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

# SendKeys goes to the focused window, so focus has to be forced first. If this is
# skipped the keys land wherever the shell was and nothing happens.
$shell = New-Object -ComObject wscript.shell
if (-not $shell.AppActivate($proc.Id)) {
    Write-Warning "Could not focus pid $($proc.Id); the keys may go elsewhere."
}
Start-Sleep -Milliseconds 700

# Up, then Enter, pause, Enter: select Continue on the main menu, then clear the
# prompt that comes after the world starts loading.
[System.Windows.Forms.SendKeys]::SendWait('{UP}')
Start-Sleep -Milliseconds $BetweenKeysMs
[System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
Start-Sleep -Seconds $BetweenKeysMs
[System.Windows.Forms.SendKeys]::SendWait('{ENTER}')

Write-Host 'Keys sent. Give it ~20 s to load the save, then check VaultCraft.log in the game folder.'
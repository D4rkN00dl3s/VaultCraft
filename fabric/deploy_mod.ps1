# Deploy the built mod into the Prism instance that pairs with the real game.
#
#     .\deploy_mod.ps1
#
# gradlew build produces the jar but does not install it anywhere, and the Prism instance is what
# runs alongside Fallout: New Vegas. So every rebuild needs this, and forgetting it is the kind of
# mistake that looks like a broken link rather than a missing file: the game heartbeats happily and
# Minecraft never opens the mapping, because the mod it is running is yesterday's build.
#
# Fails loudly rather than silently, because "the mod is not there" and "the mod is broken" produce
# the same symptom - a mapping with minecraft pid 0.

$ErrorActionPreference = 'Stop'

$instance = Join-Path $env:APPDATA 'PrismLauncher\instances\26.3\minecraft\mods'
$jar      = Get-ChildItem (Join-Path $PSScriptRoot 'build\libs') -Filter 'skycraft-*.jar' |
            Where-Object { $_.Name -notlike '*-sources.jar' } |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1

if (-not $jar) {
    throw 'no built jar in fabric\build\libs - run: gradlew build --no-configuration-cache'
}
if (-not (Test-Path $instance)) {
    throw "no mods folder at $instance - is the Prism instance where you think it is?"
}

# Overwrite in place and drop the stale build. Leaving an older jar beside a newer one makes Fabric
# load both, which fails with a duplicate-mod error that says nothing about which version is current.
Get-ChildItem $instance -Filter 'skycraft-*.jar' -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ne $jar.Name } | Remove-Item -Force

Copy-Item $jar.FullName (Join-Path $instance $jar.Name) -Force

Write-Host ("deployed {0} ({1:N0} bytes, built {2})" -f $jar.Name, $jar.Length, $jar.LastWriteTime)
Get-ChildItem $instance -Filter '*.jar' | ForEach-Object { Write-Host "  $($_.Name)" }
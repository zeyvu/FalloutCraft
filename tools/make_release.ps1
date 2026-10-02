# FalloutCraft: package a release into dist\ from the last builds.
#   1. cd commonlibf4-template; xmake build -r        (the Fallout 4 plugin)
#   2. cd fabric; .\gradlew build                     (the Minecraft mod)
#   3. powershell -ExecutionPolicy Bypass -File tools\make_release.ps1
# Makes dist\FalloutCraft-<version>-FO4.zip (MO2/Vortex), dist\commonlibf4-template.dll and
# dist\falloutcraft-<version>.jar. The version comes from fabric\gradle.properties.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$version = (Select-String -Path "$root\fabric\gradle.properties" -Pattern '^version=(.+)$').Matches[0].Groups[1].Value.Trim()
$dll = "$root\commonlibf4-template\build\windows\x64\release\commonlibf4-template.dll"
$jar = "$root\fabric\build\libs\falloutcraft-$version.jar"
foreach ($f in @($dll, $jar)) { if (-not (Test-Path $f)) { throw "Missing $f - build it first." } }

$dist = "$root\dist"
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$stage = Join-Path $env:TEMP "falloutcraft-release"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path "$stage\Data\F4SE\Plugins" | Out-Null
Copy-Item $dll "$stage\Data\F4SE\Plugins\"
@"
FalloutCraft $version - Fallout 4 side (F4SE plugin)
Requires: Fallout 4 1.11.240 (Next-Gen), F4SE 0.7.9+, Address Library for F4SE (All in One).

Mod Organizer 2 / Vortex: install this zip as a mod and enable it.
Manual: copy F4SE\Plugins\commonlibf4-template.dll into <Fallout 4>\Data\F4SE\Plugins\

Then run Minecraft 26.3 (Fabric) with falloutcraft-$version.jar FIRST,
then start Fallout 4 through f4se_loader.exe (or MO2's F4SE entry).

Source and full instructions: https://github.com/zeyvu/FalloutCraft
Based on SkyCraft by chasmlol - https://github.com/chasmlol/SkyCraft (MIT)
"@ | Set-Content -Encoding ASCII "$stage\Data\FalloutCraft_README.txt"

$zip = "$dist\FalloutCraft-$version-FO4.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage\Data" -DestinationPath $zip
Copy-Item -Force $dll "$dist\"
Copy-Item -Force $jar "$dist\"
Copy-Item -Force $dll "$root\FO4_Release\Data\F4SE\Plugins\"
Remove-Item -Recurse -Force $stage
Write-Host "FalloutCraft $version packaged in $dist :"
Get-ChildItem $dist -Filter "*$version*" | ForEach-Object { Write-Host "  $($_.Name)" }
Write-Host "  commonlibf4-template.dll"

[CmdletBinding()]
param(
    [switch]$Conan,
    [switch]$Help,
    [Parameter(ValueFromRemainingArguments=$true)][string[]]$Arguments
)
$ErrorActionPreference = 'Stop'
foreach ($argument in $Arguments) {
    switch ($argument) {
        '--conan' { $Conan = $true }
        '--help' { $Help = $true }
        default { throw "Unknown argument: $argument" }
    }
}
if ($Help) {
    Write-Output 'Usage: ./scripts/dev/bootstrap-cpp.ps1 [--conan]'
    return
}
if ($Conan) {
    if (-not (Get-Command conan -ErrorAction SilentlyContinue)) { throw 'Install Conan 2 first.' }
    & conan profile detect
    if ($LASTEXITCODE -ne 0) { throw 'conan profile detect failed.' }
    Write-Output 'In a standalone sample: conan install . --output-folder=build/conan --build=missing -s build_type=Debug'
    Write-Output 'Then: cmake --preset conan'
    return
}
if (-not $env:VCPKG_ROOT) {
    $env:VCPKG_ROOT = Join-Path ([Environment]::GetFolderPath('UserProfile')) '.cache/zlink/vcpkg'
}
if (-not (Test-Path $env:VCPKG_ROOT)) {
    New-Item -ItemType Directory -Force -Path (Split-Path $env:VCPKG_ROOT -Parent) | Out-Null
    & git clone --depth 1 https://github.com/microsoft/vcpkg.git $env:VCPKG_ROOT
    if ($LASTEXITCODE -ne 0) { throw 'vcpkg clone failed.' }
}
if (-not (Test-Path (Join-Path $env:VCPKG_ROOT 'scripts/buildsystems/vcpkg.cmake'))) {
    throw "VCPKG_ROOT is not a vcpkg checkout: $env:VCPKG_ROOT"
}
$vcpkgExe = Join-Path $env:VCPKG_ROOT 'vcpkg.exe'
if (-not (Test-Path $vcpkgExe)) {
    & (Join-Path $env:VCPKG_ROOT 'bootstrap-vcpkg.bat') -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw 'vcpkg bootstrap failed.' }
}
$overlay = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'vcpkg/ports'
if ((Test-Path $overlay) -and ($env:VCPKG_OVERLAY_PORTS -split [IO.Path]::PathSeparator) -notcontains $overlay) {
    if ($env:VCPKG_OVERLAY_PORTS) { $overlay += [IO.Path]::PathSeparator + $env:VCPKG_OVERLAY_PORTS }
    $env:VCPKG_OVERLAY_PORTS = $overlay
}
& $vcpkgExe version
if ($LASTEXITCODE -ne 0) { throw 'vcpkg version failed.' }
Write-Output "VCPKG_ROOT=$env:VCPKG_ROOT"
Write-Output 'Next: cmake --preset dev (workspace) or cmake --preset vs2022 / windows-ninja (sample).'

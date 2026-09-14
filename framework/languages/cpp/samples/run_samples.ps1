[CmdletBinding()]
param(
    [string]$BuildDir,
    [string]$LocalPackageRoot,
    [string]$CorePackagePrefix,
    [string]$VcpkgInstalledDir,
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string[]]$Sample = @()
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ScriptDir = $PSScriptRoot
$CppRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path
. (Join-Path $CppRoot "windows-build-common.ps1")
$Inputs = Resolve-ZlinkCppWindowsBuildInputs -CppRoot $CppRoot -BuildDir $BuildDir `
    -LocalPackageRoot $LocalPackageRoot -CorePackagePrefix $CorePackagePrefix `
    -VcpkgInstalledDir $VcpkgInstalledDir
$env:ZLINK_CPP_BUILD_DIR = $Inputs.BuildDir
$env:ZLINK_CPP_BUILD_CONFIGURATION = $Configuration

if ($env:OS -eq "Windows_NT") {
    $CoreRuntimeDir = Join-Path $Inputs.CorePackagePrefix "bin"
    $RuntimeDirs = @(@(
        $CoreRuntimeDir,
        (Join-Path $Inputs.CppPackagePrefix "bin"),
        (Join-Path $Inputs.VcpkgInstalledDir "x64-windows/bin")
    ) | Where-Object { Test-Path $_ } | ForEach-Object { (Resolve-Path $_).Path })
    if (-not (Test-Path (Join-Path $CoreRuntimeDir "zlink.dll"))) {
        throw "Missing Core $($Inputs.CoreVersion) Windows runtime under $($Inputs.CorePackagePrefix)"
    }
    $env:PATH = ($RuntimeDirs -join ";") + ";" + $env:PATH
}

$SampleNames = if ($Sample.Count -gt 0) {
    $Sample
} else {
    @("TicTacToe", "Bingo", "DeliveryDispatch", "SupportChat", "GameQuest", "ShoppingMall", "ZoneWorld")
}

foreach ($Name in $SampleNames) {
    $RunnerPath = Join-Path $ScriptDir "$Name/run_sample.ps1"
    if (-not (Test-Path $RunnerPath)) {
        throw "Missing C++ sample runner: $RunnerPath"
    }
    Write-Host "C++ sample $Name result=running"
    & $RunnerPath
    if ($LASTEXITCODE -ne 0) {
        throw "C++ sample $Name failed with exit code $LASTEXITCODE."
    }
}

Write-Host "sample all result=passed"

[CmdletBinding()]
param(
    [string]$BuildDir,
    [string]$LocalPackageRoot,
    [string]$VcpkgInstalledDir,
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string[]]$Sample = @()
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ScriptDir = $PSScriptRoot
$CppRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$RepositoryRoot = (Resolve-Path (Join-Path $CppRoot "../../..")).Path
if ($BuildDir) {
    $env:ZLINK_CPP_BUILD_DIR = $BuildDir
}
$env:ZLINK_CPP_BUILD_CONFIGURATION = $Configuration

if ($env:OS -eq "Windows_NT") {
    if (-not $LocalPackageRoot) {
        $LocalPackageRoot = if ($env:ZLINK_LOCAL_PACKAGE_ROOT) {
            $env:ZLINK_LOCAL_PACKAGE_ROOT
        } elseif (Test-Path (Join-Path $RepositoryRoot ".artifacts/cpp-clean-0.17.0-package")) {
            Join-Path $RepositoryRoot ".artifacts/cpp-clean-0.17.0-package"
        } else {
            Join-Path $RepositoryRoot ".artifacts/windows"
        }
    }
    if (-not $VcpkgInstalledDir) {
        $VcpkgInstalledDir = if (Test-Path (Join-Path $RepositoryRoot ".artifacts/windows-vcpkg-installed")) {
            Join-Path $RepositoryRoot ".artifacts/windows-vcpkg-installed"
        } else {
            Join-Path $RepositoryRoot ".artifacts/windows/vcpkg-installed"
        }
    }

    $CoreRuntimeDir = Join-Path $LocalPackageRoot "install/zlink-core/0.17.0/bin"
    $RuntimeDirs = @(@(
        $CoreRuntimeDir,
        (Join-Path $LocalPackageRoot "install/zlink-cpp/0.17.0/bin"),
        (Join-Path $VcpkgInstalledDir "x64-windows/bin")
    ) | Where-Object { Test-Path $_ } | ForEach-Object { (Resolve-Path $_).Path })
    if (-not (Test-Path (Join-Path $CoreRuntimeDir "zlink.dll"))) {
        throw "Missing Core 0.17.0 Windows runtime under $LocalPackageRoot"
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

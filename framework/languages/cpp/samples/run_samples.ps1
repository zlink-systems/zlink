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

function Get-ZlinkVersion {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Key
    )
    $VersionMatches = @(Select-String -LiteralPath $Path -Pattern "^$([regex]::Escape($Key))=(\d+\.\d+\.\d+)$")
    if ($VersionMatches.Count -ne 1) {
        throw "Expected exactly one canonical $Key version in $Path."
    }
    return $VersionMatches[0].Matches[0].Groups[1].Value
}

function Get-StableBuildToken {
    param([Parameter(Mandatory = $true)][string]$Path)
    $Sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $Bytes = [Text.Encoding]::UTF8.GetBytes($Path.ToLowerInvariant())
        return ([BitConverter]::ToString($Sha256.ComputeHash($Bytes), 0, 4)).Replace("-", "").ToLowerInvariant()
    } finally {
        $Sha256.Dispose()
    }
}

$ScriptDir = $PSScriptRoot
$CppRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$RepositoryRoot = (Resolve-Path (Join-Path $CppRoot "../../..")).Path
$CoreVersion = Get-ZlinkVersion -Path (Join-Path $RepositoryRoot "VERSION") -Key "LIBZLINK_VERSION"
$BindingVersion = Get-ZlinkVersion -Path (Join-Path $RepositoryRoot "bindings/cpp/VERSION") -Key "ZLINK_BINDING_VERSION"
$FrameworkVersion = Get-ZlinkVersion -Path (Join-Path $CppRoot "VERSION") -Key "ZLINK_FRAMEWORK_VERSION"
$CleanPackageRoot = Join-Path $RepositoryRoot ".artifacts/cpp-clean-$BindingVersion-package"

if (-not $BuildDir) {
    $BuildDrive = Split-Path -Qualifier $RepositoryRoot
    if (-not $BuildDrive) {
        $BuildDrive = [IO.Path]::GetTempPath()
    }
    $BuildDir = Join-Path $BuildDrive ".zlink-build/cpp-$(Get-StableBuildToken -Path $RepositoryRoot)"
}
$env:ZLINK_CPP_BUILD_DIR = $BuildDir
$env:ZLINK_CPP_BUILD_CONFIGURATION = $Configuration

if ($env:OS -eq "Windows_NT") {
    if (-not $LocalPackageRoot) {
        $LocalPackageRoot = if ($env:ZLINK_LOCAL_PACKAGE_ROOT) {
            $env:ZLINK_LOCAL_PACKAGE_ROOT
        } elseif (Test-Path $CleanPackageRoot) {
            $CleanPackageRoot
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

    $CoreRuntimeDir = Join-Path $LocalPackageRoot "install/zlink-core/$CoreVersion/bin"
    $RuntimeDirs = @(@(
        $CoreRuntimeDir,
        (Join-Path $LocalPackageRoot "install/zlink-cpp/$BindingVersion/bin"),
        (Join-Path $LocalPackageRoot "install/zlink-framework-cpp/$FrameworkVersion/bin"),
        (Join-Path $VcpkgInstalledDir "x64-windows/bin")
    ) | Where-Object { Test-Path $_ } | ForEach-Object { (Resolve-Path $_).Path })
    if (-not (Test-Path (Join-Path $CoreRuntimeDir "zlink.dll"))) {
        throw "Missing Core $CoreVersion Windows runtime under $LocalPackageRoot"
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

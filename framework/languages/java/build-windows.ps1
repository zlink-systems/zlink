[CmdletBinding()]
param(
    [string]$LocalPackageRoot = "",
    [switch]$RunTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$JavaRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $JavaRoot "../../.."))
if (-not $LocalPackageRoot) {
    $LocalPackageRoot = Join-Path $RepositoryRoot ".artifacts/windows"
}
$LocalPackageRoot = [System.IO.Path]::GetFullPath($LocalPackageRoot)
if (-not (Test-Path -LiteralPath (Join-Path $LocalPackageRoot "maven") -PathType Container)) {
    throw "Local Maven package repository was not found: $LocalPackageRoot/maven"
}

$env:ZLINK_LOCAL_PACKAGE_ROOT = $LocalPackageRoot

function Invoke-GradleBuild {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectDirectory,
        [Parameter(Mandatory = $true)][string[]]$Tasks
    )

    & (Join-Path $JavaRoot "gradlew.bat") `
        --no-daemon --no-parallel --max-workers=2 `
        -p $ProjectDirectory @Tasks
    if ($LASTEXITCODE -ne 0) {
        throw "Gradle build failed in $ProjectDirectory"
    }
}

Invoke-GradleBuild -ProjectDirectory $JavaRoot -Tasks @("assemble")

$SamplesRoot = Join-Path $JavaRoot "samples"
Invoke-GradleBuild -ProjectDirectory $SamplesRoot -Tasks @("installDist")

. (Join-Path $SamplesRoot "redis-common.ps1")
Optimize-ZlinkSampleWindowsLaunchers -Root $SamplesRoot

if ($RunTests) {
    Invoke-GradleBuild -ProjectDirectory $JavaRoot -Tasks @("check")
}

Write-Output "Java/Kotlin Framework assemble and all sample installDist tasks completed"
if (-not $RunTests) {
    Write-Output "Tests were not run. Pass -RunTests to run the independent verification phase."
}

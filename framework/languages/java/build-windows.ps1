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
. (Join-Path $JavaRoot "local-package-common.ps1")
if (-not $LocalPackageRoot) {
    $LocalPackageRoot = Join-Path $RepositoryRoot ".artifacts/windows"
}
$LocalPackageRoot = [System.IO.Path]::GetFullPath($LocalPackageRoot)
$BindingVersion = Assert-ZlinkJavaLocalBindingPackage `
    -RepositoryRoot $RepositoryRoot `
    -LocalPackageRoot $LocalPackageRoot

$env:ZLINK_LOCAL_PACKAGE_ROOT = $LocalPackageRoot
$env:ZLINK_JAVA_REQUIRE_LOCAL_BINDING = "true"
Write-Output "Using exact local Java binding package $BindingVersion from $LocalPackageRoot/maven"

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

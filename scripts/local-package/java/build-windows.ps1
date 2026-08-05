param()

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (git -C $ScriptDir rev-parse --show-toplevel).Trim()

if ($env:ZLINK_LOCAL_PACKAGE_ROOT) {
    $ArtifactRoot = $env:ZLINK_LOCAL_PACKAGE_ROOT
} else {
    $ArtifactRoot = Join-Path $RepoRoot ".artifacts/windows"
}

$OutDir = Join-Path $ArtifactRoot "maven"
$BindingsDir = Join-Path $RepoRoot "bindings/java"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

$PreviousRepo = $env:MAVEN_REPOSITORY_URL
$env:MAVEN_REPOSITORY_URL = (New-Object System.Uri($OutDir)).AbsoluteUri
try {
    Push-Location $BindingsDir
    .\gradlew.bat publish
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
    $env:MAVEN_REPOSITORY_URL = $PreviousRepo
}

Write-Host "-- Java local Maven repository output: $OutDir"

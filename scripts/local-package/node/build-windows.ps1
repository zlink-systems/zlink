param()

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (git -C $ScriptDir rev-parse --show-toplevel).Trim()

if ($env:ZLINK_LOCAL_PACKAGE_ROOT) {
    $ArtifactRoot = $env:ZLINK_LOCAL_PACKAGE_ROOT
} else {
    $ArtifactRoot = Join-Path $RepoRoot ".artifacts/windows"
}

$OutDir = Join-Path $ArtifactRoot "npm"
$BindingsDir = Join-Path $RepoRoot "bindings/node"
$PackageMode = if ($env:ZLINK_NODE_PACKAGE_MODE) { $env:ZLINK_NODE_PACKAGE_MODE } else { "source" }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Push-Location $BindingsDir
try {
    if ($env:ZLINK_SKIP_NPM_CI -ne "true") {
        npm ci
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    }
    npm run build
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
    npm run rebuild-native
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
    switch ($PackageMode) {
        "source" {
        }
        "prebuild" {
            npm run verify:prebuilds
            if ($LASTEXITCODE -ne 0) {
                exit $LASTEXITCODE
            }
        }
        default {
            Write-Error "Unknown ZLINK_NODE_PACKAGE_MODE: $PackageMode. Expected source or prebuild."
        }
    }
    npm pack --pack-destination $OutDir
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
}

Write-Host "-- Node local npm tarball output: $OutDir"

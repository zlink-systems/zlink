param(
    [string]$RepositoryRoot = "",
    [string]$LocalPackageRoot = "",
    [switch]$SkipSamples
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = (Resolve-Path (Join-Path $scriptDir "../../..")).Path
} else {
    $RepositoryRoot = (Resolve-Path $RepositoryRoot).Path
}
if ([string]::IsNullOrWhiteSpace($LocalPackageRoot)) {
    $LocalPackageRoot = if ([string]::IsNullOrWhiteSpace($env:ZLINK_LOCAL_PACKAGE_ROOT)) {
        Join-Path $RepositoryRoot ".artifacts/windows"
    } else {
        $env:ZLINK_LOCAL_PACKAGE_ROOT
    }
}
$LocalPackageRoot = [System.IO.Path]::GetFullPath($LocalPackageRoot)
$nodeRoot = Join-Path $RepositoryRoot "framework/languages/node"
$npmRoot = Join-Path $LocalPackageRoot "npm"
$npm = (Get-Command npm.cmd -ErrorAction Stop).Source
$node = (Get-Command node.exe -ErrorAction Stop).Source

function Invoke-Checked {
    param(
        [string]$Executable,
        [string[]]$Arguments,
        [string]$WorkingDirectory
    )
    Push-Location $WorkingDirectory
    try {
        & $Executable @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "Command failed with exit code ${LASTEXITCODE}: $Executable $($Arguments -join ' ')"
        }
    } finally {
        Pop-Location
    }
}

$frameworkManifest = Get-Content -Raw -Encoding utf8 (Join-Path $nodeRoot "packages/framework/package.json") | ConvertFrom-Json
$httpManifest = Get-Content -Raw -Encoding utf8 (Join-Path $nodeRoot "packages/http-client/package.json") | ConvertFrom-Json
$bindingVersion = $frameworkManifest.dependencies.'@zlink-systems/zlink'
$coreVersion = (Select-String -LiteralPath (Join-Path $RepositoryRoot "VERSION") -Pattern "^LIBZLINK_VERSION=(.+)$").Matches.Groups[1].Value
$httpVersion = $httpManifest.version
$bindingPackage = Join-Path $npmRoot "zlink-systems-zlink-$bindingVersion.tgz"
$httpPackage = Join-Path $npmRoot "zlink-systems-http-client-$httpVersion.tgz"
$materializedHttpPackage = Join-Path $RepositoryRoot ".artifacts\node-install\npm\zlink-systems-http-client-$httpVersion.tgz"
if (-not (Test-Path -LiteralPath $bindingPackage -PathType Leaf)) {
    throw "Node binding local package is missing: $bindingPackage"
}
New-Item -ItemType Directory -Force -Path $npmRoot | Out-Null

# Bootstrap the workspace with the exact local binding and the HTTP client source.
# The published HTTP client tarball does not exist until this first compilation finishes,
# so explicit package arguments avoid rewriting package.json or package-lock.json.
Invoke-Checked $npm @(
    "install", "--no-save", "--no-package-lock", "--ignore-scripts", "--no-audit", "--no-fund",
    $bindingPackage, (Join-Path $nodeRoot "packages/http-client")
) $nodeRoot
Invoke-Checked $node @("node_modules/typescript/bin/tsc", "-b", "packages/http-client") $nodeRoot
Invoke-Checked $npm @("pack", "--pack-destination", $npmRoot, ".\packages\http-client") $nodeRoot
if (-not (Test-Path -LiteralPath $httpPackage -PathType Leaf)) {
    throw "Node HTTP client local package was not created: $httpPackage"
}
$previousLocalPackageRoot = $env:ZLINK_LOCAL_PACKAGE_ROOT
try {
    $env:ZLINK_LOCAL_PACKAGE_ROOT = $LocalPackageRoot
    Invoke-Checked $node @("scripts/materialize-local-http-client-package.mjs") $nodeRoot
} finally {
    $env:ZLINK_LOCAL_PACKAGE_ROOT = $previousLocalPackageRoot
}

# Reinstall both local tarballs so runtime/sample verification uses packaged output.
Invoke-Checked $npm @(
    "install", "--no-save", "--no-package-lock", "--ignore-scripts", "--no-audit", "--no-fund",
    $bindingPackage, $materializedHttpPackage
) $nodeRoot
$bindingVerification = @'
const binding = require('@zlink-systems/zlink');
const fs = require('node:fs');
const path = require('node:path');
const manifestPath = path.join(path.dirname(require.resolve('@zlink-systems/zlink')), '..', 'package.json');
const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
const expectedCore = '__ZLINK_CORE_VERSION__';
const expectedBinding = '__ZLINK_BINDING_VERSION__';
const nativeVersion = binding.version().join('.');
if (manifest.version !== expectedBinding || nativeVersion !== expectedCore) {
  throw new Error('Expected Node binding package ' + expectedBinding + ' / Core ' + expectedCore + ', package=' + manifest.version + ', native=' + nativeVersion);
}
'@.Replace('__ZLINK_CORE_VERSION__', $coreVersion).Replace('__ZLINK_BINDING_VERSION__', $bindingVersion)
Invoke-Checked $node @("-e", $bindingVerification) $nodeRoot
Invoke-Checked $npm @("run", "build") $nodeRoot
Invoke-Checked $node @("--test", "test/smoke/binding-smoke.test.js") $nodeRoot

if (-not $SkipSamples) {
    & (Join-Path $nodeRoot "samples/build_samples.ps1") -SkipFrameworkBuild
    if (-not $?) { throw "Node sample build failed." }
}

Write-Output "Node Framework Windows build passed."
Write-Output "binding=$bindingPackage"
Write-Output "httpClient=$materializedHttpPackage"

# PowerShell version definitions for Windows builds

param()

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$versionFile = Join-Path $repoRoot "VERSION"
$script:LIBZLINK_VERSION = (Select-String -LiteralPath $versionFile -Pattern "^LIBZLINK_VERSION=(.+)$").Matches.Groups[1].Value
if ($script:LIBZLINK_VERSION -notmatch "^[0-9]+\.[0-9]+\.[0-9]+$") {
    throw "Repository VERSION has no valid LIBZLINK_VERSION: $versionFile"
}

# Export variables to caller scope
if ($MyInvocation.InvocationName -ne '.') {
    $global:LIBZLINK_VERSION = $script:LIBZLINK_VERSION
}

Write-Host "==================================="
Write-Host "Build Configuration"
Write-Host "==================================="
Write-Host "libzlink version:    $script:LIBZLINK_VERSION"
Write-Host "==================================="

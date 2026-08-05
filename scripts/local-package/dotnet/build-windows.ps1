param(
    [Parameter(Mandatory = $true)][string] $CorePrefix,
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-f]{64}$')][string] $CoreProvenanceSha256,
    [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-f]{64}$')][string] $CoreCandidateManifestSha256,
    [string] $Configuration = $env:CONFIGURATION
)

$ErrorActionPreference = "Stop"
if (-not $Configuration) { $Configuration = "Release" }
if (-not [System.IO.Path]::IsPathFullyQualified($CorePrefix)) {
    throw "CorePrefix must be an absolute approved Core 11 package prefix"
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (git -C $ScriptDir rev-parse --show-toplevel).Trim()
$ArtifactRoot = if ($env:ZLINK_LOCAL_PACKAGE_ROOT) { $env:ZLINK_LOCAL_PACKAGE_ROOT } else { Join-Path $RepoRoot ".artifacts/windows" }
$Manifest = Join-Path $CorePrefix "share/zlink/core-package-provenance.json"
if (-not (Test-Path -LiteralPath $Manifest -PathType Leaf)) { throw "Core package provenance is missing: $Manifest" }
if ((Get-FileHash -LiteralPath $Manifest -Algorithm SHA256).Hash.ToLowerInvariant() -ne $CoreProvenanceSha256) {
    throw "Core package provenance SHA-256 does not match the approved input"
}

$Provenance = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
if ($Provenance.package -ne "zlink-core" -or $Provenance.version -notmatch '^11\.[0-9]+\.[0-9]+$' -or
    $Provenance.candidate.ledgerId -ne "V11-M3-CORE-VERIFY" -or
    $Provenance.candidate.manifestSha256 -ne $CoreCandidateManifestSha256) {
    throw "Core package identity, version, or candidate manifest does not match the approved input"
}

$Version = $Provenance.version
$NativeRoot = Join-Path $CorePrefix "lib"
$ExactRuntime = Join-Path $NativeRoot "libzlink.so.$Version"
$RuntimeEntry = $Provenance.files | Where-Object { $_.path -eq "lib/libzlink.so.$Version" }
if (-not $RuntimeEntry -or -not (Test-Path -LiteralPath $ExactRuntime -PathType Leaf)) {
    throw "Approved exact Core runtime is missing"
}
if ((Get-FileHash -LiteralPath $ExactRuntime -Algorithm SHA256).Hash.ToLowerInvariant() -ne $RuntimeEntry.sha256) {
    throw "Core runtime SHA-256 does not match provenance"
}

$Project = Join-Path $RepoRoot "bindings/dotnet/src/Zlink/Zlink.csproj"
$ProjectVersion = ([xml](Get-Content -LiteralPath $Project -Raw)).Project.PropertyGroup.Version | Select-Object -First 1
$PackageSemVer = [version]$ProjectVersion
$CoreSemVer = [version]$Version
if ($PackageSemVer.Major -ne $CoreSemVer.Major -or
    $PackageSemVer.Minor -ne $CoreSemVer.Minor -or
    $PackageSemVer.Build -lt $CoreSemVer.Build) {
    throw ".NET package $ProjectVersion must use Core $Version major.minor and an equal or newer patch"
}
$OutDir = Join-Path $ArtifactRoot "nuget"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

dotnet pack $Project -c $Configuration -o $OutDir `
    -p:ZLinkLinuxX64NativeRoot=$NativeRoot `
    -p:ZLinkCoreVersion=$Version `
    -p:ZLinkCoreProvenancePath=$Manifest
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "-- .NET local NuGet package output: $OutDir"
Write-Host "-- Approved Core candidate: $CoreCandidateManifestSha256"

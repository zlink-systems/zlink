param(
  [string]$Version = "",
  [string]$Platform = "windows-x64",
  [string]$CacheDir = "",
  [switch]$Force
)

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$repoVersion = (Select-String -LiteralPath (Join-Path $repoRoot "VERSION") -Pattern "^LIBZLINK_VERSION=(.+)$").Matches.Groups[1].Value
if ([string]::IsNullOrWhiteSpace($Version)) {
  $Version = $repoVersion
}
if ($Version -notmatch "^[0-9]+\.[0-9]+\.[0-9]+$") {
  throw "Core release version must be MAJOR.MINOR.PATCH: $Version"
}
if ($Version -ne $repoVersion) {
  throw "Core release version $Version must match repository VERSION $repoVersion"
}

$supportedPlatforms = @("windows-x64", "windows-arm64", "linux-x64", "linux-arm64", "macos-x64", "macos-arm64")
if ($supportedPlatforms -notcontains $Platform) {
  throw "Unsupported Core release platform: $Platform"
}
if ($Platform -notlike "windows-*") {
  throw "PowerShell Core release fetcher is intended for Windows assets"
}

if ([string]::IsNullOrWhiteSpace($CacheDir)) {
  $localAppData = [Environment]::GetFolderPath("LocalApplicationData")
  if ([string]::IsNullOrWhiteSpace($localAppData)) {
    $localAppData = Join-Path $repoRoot ".artifacts"
  }
  $CacheDir = Join-Path $localAppData "zlink\core"
}
if (-not [IO.Path]::IsPathRooted($CacheDir)) {
  throw "CacheDir must be an absolute path: $CacheDir"
}
$cacheRoot = [IO.Path]::GetFullPath($CacheDir)
$prefix = Join-Path (Join-Path $cacheRoot $Version) $Platform
$manifestPath = Join-Path $prefix "share\zlink\core-package-provenance.json"

if (-not $Force -and (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
  try {
    $existing = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($existing.version -eq $Version -and $existing.package -eq "zlink-core") {
      Write-Output $prefix
      exit 0
    }
  } catch {
    # Recreate an incomplete cache entry below.
  }
}

$releaseTag = "core/v$Version"
$releaseBase = "https://github.com/zlink-systems/zlink/releases/download/$releaseTag"
$binaryName = "libzlink-$Platform"
$binaryArchiveName = "$binaryName.zip"
$cacheParent = Split-Path -Parent $prefix
New-Item -ItemType Directory -Path $cacheParent -Force | Out-Null
$work = Join-Path $cacheParent (".download." + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work -Force | Out-Null

function Download-ReleaseFile([string]$Name, [string]$Destination) {
  $url = "$releaseBase/$Name"
  Write-Host "Downloading $url"
  Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $Destination
}

function Get-ReleaseValue([string]$Key, [string]$Path) {
  $line = Get-Content -LiteralPath $Path | Where-Object { $_ -match ("^" + [regex]::Escape($Key) + "=") } | Select-Object -First 1
  if ($null -eq $line) {
    throw "Release provenance is missing $Key"
  }
  return $line.Substring($Key.Length + 1)
}

function Get-Sha256([string]$Path) {
  return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

try {
  $binaryArchive = Join-Path $work $binaryArchiveName
  $sourceArchive = Join-Path $work "zlink-$Version-source.tar.gz"
  $checksums = Join-Path $work "checksums.txt"
  $releaseProvenance = Join-Path $work "release-provenance.txt"
  Download-ReleaseFile $binaryArchiveName $binaryArchive
  Download-ReleaseFile "zlink-$Version-source.tar.gz" $sourceArchive
  Download-ReleaseFile "checksums.txt" $checksums
  Download-ReleaseFile "release-provenance.txt" $releaseProvenance

  if ((Get-ReleaseValue "tag" $releaseProvenance) -ne $releaseTag) {
    throw "Release provenance tag does not match $releaseTag"
  }
  if ((Get-ReleaseValue "runtime_version" $releaseProvenance) -ne $Version) {
    throw "Release provenance version does not match $Version"
  }
  $expectedChecksumsSha = Get-ReleaseValue "checksums_sha256" $releaseProvenance
  if ((Get-Sha256 $checksums) -ne $expectedChecksumsSha.ToLowerInvariant()) {
    throw "Release checksums.txt SHA-256 mismatch"
  }
  $expectedSourceSha = Get-ReleaseValue "source_archive_sha256" $releaseProvenance
  if ((Get-Sha256 $sourceArchive) -ne $expectedSourceSha.ToLowerInvariant()) {
    throw "Release source archive SHA-256 mismatch"
  }

  $binaryRoot = Join-Path $work "binary"
  Expand-Archive -LiteralPath $binaryArchive -DestinationPath $binaryRoot
  $binaryPrefix = Join-Path $binaryRoot $binaryName
  if (-not (Test-Path -LiteralPath $binaryPrefix -PathType Container)) {
    throw "Core release archive has an unexpected root: $binaryName"
  }

  $checksumPattern = "^([0-9a-fA-F]{64})\s+\./" + [regex]::Escape($binaryName) + "/(.+)$"
  $verifiedFiles = 0
  foreach ($line in Get-Content -LiteralPath $checksums) {
    if ($line -match $checksumPattern) {
      $expectedHash = $Matches[1].ToLowerInvariant()
      $relativePath = $Matches[2].Replace("/", [IO.Path]::DirectorySeparatorChar)
      $filePath = Join-Path (Join-Path $binaryRoot $binaryName) $relativePath
      if (-not (Test-Path -LiteralPath $filePath -PathType Leaf)) {
        throw "Release checksum file names a missing asset: $relativePath"
      }
      if ((Get-Sha256 $filePath) -ne $expectedHash) {
        throw "Release asset checksum mismatch: $relativePath"
      }
      $verifiedFiles++
    }
  }
  if ($verifiedFiles -eq 0) {
    throw "Release checksums do not contain platform $Platform"
  }

  $tarCommand = Get-Command tar.exe -ErrorAction SilentlyContinue
  if ($null -eq $tarCommand) {
    $tarCommand = Get-Command tar -ErrorAction SilentlyContinue
  }
  if ($null -eq $tarCommand) {
    throw "tar.exe is required to extract the Core source archive"
  }
  $sourceRoot = Join-Path $work "source"
  New-Item -ItemType Directory -Path $sourceRoot -Force | Out-Null
  & $tarCommand.Source -xzf $sourceArchive -C $sourceRoot "core/include"
  if ($LASTEXITCODE -ne 0) {
    throw "Failed to extract the Core source archive"
  }
  $sourceInclude = Join-Path $sourceRoot "core\include"
  if (-not (Test-Path -LiteralPath $sourceInclude -PathType Container)) {
    throw "Core source archive does not contain core/include"
  }

  $stage = Join-Path $work "prefix"
  $stageInclude = Join-Path $stage "include"
  $stageShare = Join-Path $stage "share\zlink"
  New-Item -ItemType Directory -Path $stageInclude -Force | Out-Null
  New-Item -ItemType Directory -Path $stageShare -Force | Out-Null
  Copy-Item -Path (Join-Path $sourceInclude "*") -Destination $stageInclude -Recurse -Force
  Copy-Item -Path (Join-Path (Join-Path $binaryPrefix "include") "*") -Destination $stageInclude -Recurse -Force
  Copy-Item -LiteralPath $checksums -Destination (Join-Path $stageShare "release-checksums.txt")
  Copy-Item -LiteralPath $releaseProvenance -Destination (Join-Path $stageShare "release-provenance.txt")

  $stageBin = Join-Path $stage "bin"
  $stageLib = Join-Path $stage "lib"
  New-Item -ItemType Directory -Path $stageBin -Force | Out-Null
  New-Item -ItemType Directory -Path $stageLib -Force | Out-Null
  Copy-Item -Path (Join-Path (Join-Path $binaryPrefix "bin") "*") -Destination $stageBin -Recurse -Force
  Copy-Item -Path (Join-Path (Join-Path $binaryPrefix "lib") "*") -Destination $stageLib -Recurse -Force

  $runtimePath = "bin/zlink.dll"
  $runtimeFile = Join-Path $stage ($runtimePath.Replace("/", "\"))
  if (-not (Test-Path -LiteralPath $runtimeFile -PathType Leaf)) {
    throw "Windows Core runtime is missing: $runtimePath"
  }
  $files = @(Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object {
    [ordered]@{
      path = $_.FullName.Substring($stage.Length + 1).Replace("\", "/")
      sha256 = Get-Sha256 $_.FullName
    }
  }) | Sort-Object path
  $manifest = [ordered]@{
    schema = 1
    package = "zlink-core"
    version = $Version
    abiMajor = 0
    runtime = [ordered]@{
      path = $runtimePath
      sha256 = Get-Sha256 $runtimeFile
      soname = $null
    }
    source = [ordered]@{
      revision = Get-ReleaseValue "source_sha" $releaseProvenance
      dirty = $false
    }
    release = [ordered]@{
      tag = $releaseTag
      checksumsSha256 = Get-Sha256 $checksums
    }
    files = $files
  }
  $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $stageShare "core-package-provenance.json") -Encoding UTF8

  if (Test-Path -LiteralPath $prefix) {
    Remove-Item -LiteralPath $prefix -Recurse -Force
  }
  Move-Item -LiteralPath $stage -Destination $prefix
  Write-Host "Core release installed: version=$Version platform=$Platform prefix=$prefix"
  Write-Output $prefix
} finally {
  if (Test-Path -LiteralPath $work) {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
  }
}

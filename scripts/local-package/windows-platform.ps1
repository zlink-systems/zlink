# SPDX-License-Identifier: MPL-2.0

function Get-ZLinkPeArchitecture {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  $resolved = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
  $stream = [IO.File]::Open($resolved, [IO.FileMode]::Open,
    [IO.FileAccess]::Read, [IO.FileShare]::Read)
  $reader = New-Object IO.BinaryReader($stream)
  try {
    if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5A4D) {
      throw "File is not a PE image: $resolved"
    }
    $stream.Position = 0x3C
    $peOffset = $reader.ReadInt32()
    if ($peOffset -lt 0 -or ($peOffset + 6) -gt $stream.Length) {
      throw "File has an invalid PE header offset: $resolved"
    }
    $stream.Position = $peOffset
    if ($reader.ReadUInt32() -ne 0x00004550) {
      throw "File is missing the PE signature: $resolved"
    }
    $machine = $reader.ReadUInt16()
    switch ($machine) {
      0x8664 { return "x64" }
      0xAA64 { return "arm64" }
      default {
        throw ("Unsupported PE machine 0x{0:X4}: {1}" -f $machine, $resolved)
      }
    }
  } finally {
    $reader.Dispose()
    $stream.Dispose()
  }
}

function Resolve-ZLinkWindowsCorePackage {
  param(
    [Parameter(Mandatory = $true)]
    [string]$CorePrefix,
    [Parameter(Mandatory = $true)]
    [string]$ExpectedVersion
  )

  $resolvedPrefix = (Resolve-Path -LiteralPath $CorePrefix -ErrorAction Stop).Path
  $manifestPath = Join-Path $resolvedPrefix "share\zlink\core-package-provenance.json"
  $runtimePath = Join-Path $resolvedPrefix "bin\zlink.dll"
  if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Core prefix is missing provenance: $manifestPath"
  }
  if (-not (Test-Path -LiteralPath $runtimePath -PathType Leaf)) {
    throw "Core prefix is missing zlink.dll: $runtimePath"
  }

  $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
  if ($manifest.package -ne "zlink-core" -or
      $manifest.version -ne $ExpectedVersion -or
      $manifest.abiMajor -ne 0) {
    throw "Core prefix provenance does not match Core ${ExpectedVersion}: $manifestPath"
  }
  if ($manifest.platform -ne "windows-x64") {
    throw "Core prefix provenance is not the supported Windows x64 platform: $manifestPath"
  }

  $runtimeHash = (Get-FileHash -LiteralPath $runtimePath -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($manifest.runtime.path -ne "bin/zlink.dll" -or
      $manifest.runtime.sha256 -ne $runtimeHash) {
    throw "Core runtime does not match its provenance: $resolvedPrefix"
  }
  $runtimeArchitecture = Get-ZLinkPeArchitecture -Path $runtimePath
  if ($runtimeArchitecture -ne "x64") {
    throw "Core runtime architecture $runtimeArchitecture does not match provenance platform $($manifest.platform)"
  }

  $target = [pscustomobject]@{
    Architecture = "x64"
    CorePlatform = "windows-x64"
    CMakePlatform = "x64"
    DotNetRid = "win-x64"
    DotNetNativeRootProperty = "ZLinkWindowsX64NativeRoot"
    JavaResourceArchitecture = "x86_64"
    NodeArchitecture = "x64"
    NodePrebuild = "win32-x64"
  }

  return [pscustomobject]@{
    Prefix = $resolvedPrefix
    ManifestPath = $manifestPath
    Manifest = $manifest
    RuntimePath = $runtimePath
    Target = $target
  }
}

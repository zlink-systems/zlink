# SPDX-License-Identifier: MPL-2.0

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "..\windows-platform.ps1")

function Assert-Equal($Expected, $Actual, [string]$Message) {
  if ($Expected -ne $Actual) {
    throw "$Message (expected=$Expected actual=$Actual)"
  }
}

function Assert-Throws([scriptblock]$Action, [string]$Pattern) {
  try {
    & $Action
  } catch {
    if ($_.Exception.Message -notmatch $Pattern) {
      throw "Expected error '$Pattern', got '$($_.Exception.Message)'"
    }
    return
  }
  throw "Expected action to fail with '$Pattern'"
}

function Write-TestPe([string]$Path, [UInt16]$Machine) {
  $bytes = New-Object byte[] 128
  $bytes[0] = 0x4D
  $bytes[1] = 0x5A
  [BitConverter]::GetBytes([int]64).CopyTo($bytes, 0x3C)
  $bytes[64] = 0x50
  $bytes[65] = 0x45
  [BitConverter]::GetBytes($Machine).CopyTo($bytes, 68)
  [IO.File]::WriteAllBytes($Path, $bytes)
}

function New-TestCorePrefix(
  [string]$Root,
  [string]$Platform,
  [UInt16]$Machine
) {
  $prefix = Join-Path $Root $Platform
  $bin = Join-Path $prefix "bin"
  $include = Join-Path $prefix "include"
  $share = Join-Path $prefix "share\zlink"
  New-Item -ItemType Directory -Force -Path $bin, $include, $share | Out-Null
  [IO.File]::WriteAllText((Join-Path $include "zlink.h"), "test")
  $runtime = Join-Path $bin "zlink.dll"
  Write-TestPe -Path $runtime -Machine $Machine
  $manifest = [ordered]@{
    schema = 1
    package = "zlink-core"
    version = "9.8.7"
    abiMajor = 0
    platform = $Platform
    runtime = [ordered]@{
      path = "bin/zlink.dll"
      sha256 = (Get-FileHash -LiteralPath $runtime -Algorithm SHA256).Hash.ToLowerInvariant()
      soname = $null
    }
  } | ConvertTo-Json -Depth 4
  [IO.File]::WriteAllText(
    (Join-Path $share "core-package-provenance.json"),
    $manifest + [Environment]::NewLine,
    (New-Object Text.UTF8Encoding($false)))
  return $prefix
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("zlink-windows-platform-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempRoot | Out-Null
try {
  $x64Prefix = New-TestCorePrefix -Root $tempRoot -Platform "windows-x64" -Machine 0x8664

  $x64 = Resolve-ZLinkWindowsCorePackage -CorePrefix $x64Prefix -ExpectedVersion "9.8.7"
  Assert-Equal "x64" $x64.Target.Architecture "x64 architecture"
  Assert-Equal "x64" $x64.Target.CMakePlatform "x64 CMake platform"
  Assert-Equal "win-x64" $x64.Target.DotNetRid "x64 .NET RID"
  Assert-Equal "win32-x64" $x64.Target.NodePrebuild "x64 Node prebuild"
  Assert-Equal "x86_64" $x64.Target.JavaResourceArchitecture "x64 Java resource"

  $unsupportedRoot = Join-Path $tempRoot "unsupported"
  New-Item -ItemType Directory -Path $unsupportedRoot | Out-Null
  $arm64Prefix = New-TestCorePrefix -Root $unsupportedRoot -Platform "windows-arm64" -Machine 0xAA64
  Assert-Throws {
    Resolve-ZLinkWindowsCorePackage -CorePrefix $arm64Prefix -ExpectedVersion "9.8.7"
  } "not the supported Windows x64 platform"

  $mismatchRoot = Join-Path $tempRoot "mismatch"
  New-Item -ItemType Directory -Path $mismatchRoot | Out-Null
  $mismatchPrefix = New-TestCorePrefix -Root $mismatchRoot -Platform "windows-x64" -Machine 0xAA64
  Assert-Throws {
    Resolve-ZLinkWindowsCorePackage -CorePrefix $mismatchPrefix -ExpectedVersion "9.8.7"
  } "runtime architecture arm64 does not match provenance platform windows-x64"

  Write-Host "Windows platform contract tests passed: x64, ARM64 rejection, PE mismatch rejection"
} finally {
  $resolvedTemp = [IO.Path]::GetFullPath($tempRoot)
  $systemTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
  if (-not $resolvedTemp.StartsWith($systemTemp, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to remove test directory outside the system temp root: $resolvedTemp"
  }
  Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
}

# SPDX-License-Identifier: MPL-2.0
#
# Owns what a Windows Core package prefix must be: the provenance platform name
# and the machine type its runtime is compiled for. Producers (release fetch,
# local rebuild) and the consumer (binding package build) dot-source this file so
# the contract is stated once instead of being restated at each call site.

# Dot-sourced into scripts with different strictness settings, so it changes no
# language mode of its own.
$script:ZlinkWindowsX64Platform = "windows-x64"
$script:ZlinkPeMachineAmd64 = 0x8664

function Get-ZlinkPeMachine {
  param([Parameter(Mandatory = $true)][string]$Path)

  $stream = [IO.File]::OpenRead($Path)
  try {
    $reader = New-Object IO.BinaryReader($stream)
    if ($reader.ReadUInt16() -ne 0x5A4D) {
      throw "Not a PE image: $Path"
    }
    $stream.Position = 0x3C
    $stream.Position = $reader.ReadUInt32()
    if ($reader.ReadUInt32() -ne 0x00004550) {
      throw "PE signature is missing: $Path"
    }
    return $reader.ReadUInt16()
  } finally {
    $stream.Dispose()
  }
}

function Assert-ZlinkWindowsX64Image {
  param([Parameter(Mandatory = $true)][string]$Path)

  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    throw "Windows x64 image is missing: $Path"
  }
  $machine = Get-ZlinkPeMachine -Path $Path
  if ($machine -ne $script:ZlinkPeMachineAmd64) {
    throw ("Windows package input must be x64: {0} reports PE machine 0x{1:X4}" -f $Path, $machine)
  }
}

function Assert-ZlinkWindowsCorePrefix {
  param(
    [Parameter(Mandatory = $true)][string]$Prefix,
    [Parameter(Mandatory = $true)]$Provenance
  )

  $platform = if ($Provenance.PSObject.Properties.Name -contains "platform") { $Provenance.platform } else { $null }
  if ($platform -ne $script:ZlinkWindowsX64Platform) {
    throw ("Core prefix must record platform {0}: {1} records '{2}'" -f
      $script:ZlinkWindowsX64Platform, $Prefix, $platform)
  }
  Assert-ZlinkWindowsX64Image -Path (Join-Path $Prefix "bin\zlink.dll")
}

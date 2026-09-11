[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CorePrefix,
    [string]$RepositoryRoot = "",
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [ValidateSet("", "x64", "arm64")]
    [string]$Architecture = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

& (Join-Path $PSScriptRoot "..\build-windows.ps1") `
    -CorePrefix $CorePrefix `
    -RepositoryRoot $RepositoryRoot `
    -Language dotnet `
    -Configuration $Configuration `
    -Architecture $Architecture

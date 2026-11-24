$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
Set-StrictMode -Version Latest
if ($PSVersionTable.PSVersion.Major -lt 7) { throw "PowerShell 7 or later is required." }
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$OutputEncoding = [Console]::OutputEncoding
Set-Location $PSScriptRoot

if ($args.Count -ne 0) { throw 'Usage: ./run-server.ps1' }
& npm.cmd ci
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& npm.cmd run build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& node --require ./dist/tracing.js dist/server.js
exit $LASTEXITCODE

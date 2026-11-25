$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'scripts/run-helpers.ps1')
Initialize-Console
Set-Location $PSScriptRoot
& node scripts/run-server.mjs --serve @args
exit $LASTEXITCODE

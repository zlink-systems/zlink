$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'scripts/run-helpers.ps1')
Initialize-Console
Set-Location $PSScriptRoot

if ($args.Count -ne 0) { throw 'Usage: ./run-server.ps1' }
New-Item -ItemType Directory -Force '.logs' | Out-Null
Assert-NoRunningServer
Build-Local
& node dist/e2e/runner.js --serve
exit $LASTEXITCODE

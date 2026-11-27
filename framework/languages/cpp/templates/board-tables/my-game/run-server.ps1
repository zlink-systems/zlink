$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
Set-StrictMode -Version Latest
$root = $PSScriptRoot
. (Join-Path $root 'scripts/run-helpers.ps1')
Initialize-Console

if ($args.Count -ne 0) { throw 'Usage: ./run-server.ps1' }
Assert-NoRunningServer
Build-Local
& (Get-Runner) --root $root --serve
exit $LASTEXITCODE

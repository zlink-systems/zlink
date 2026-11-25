$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
Set-StrictMode -Version Latest
$root = $PSScriptRoot
. (Join-Path $root 'scripts/run-helpers.ps1')
Initialize-Console

if ($args.Count -ne 0) { throw 'Usage: ./run-server.ps1' }
Assert-NoRunningServer
Write-Host 'Preparing: building the server (the first run downloads the Framework prebuilt)...'
Build-Local
Write-Host 'Build complete. Starting servers...'
& (Get-Runner) --root $root --serve
exit $LASTEXITCODE

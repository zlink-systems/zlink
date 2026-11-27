$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = $PSScriptRoot
. (Join-Path $root 'scripts/run-helpers.ps1')
if ($args.Count -gt 1 -or ($args.Count -eq 1 -and $args[0] -ne '--network-sync')) {
    throw 'Usage: run-server.cmd [--network-sync]'
}
Write-Host 'Preparing: building the server (the first run restores NuGet packages)...'
Build-Local
Write-Host 'Build complete. Starting servers...'
& dotnet $runner --root $root --serve @args
exit $LASTEXITCODE

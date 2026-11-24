$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = $PSScriptRoot
. (Join-Path $root 'scripts/run-helpers.ps1')

if ($args.Count -ne 0) { throw 'Usage: ./run-server.ps1' }
Write-Host 'Preparing: building the server (the first run restores NuGet packages)...'
Build-Local
Write-Host 'Build complete. Starting servers...'
& dotnet $runner --root $root --serve
exit $LASTEXITCODE

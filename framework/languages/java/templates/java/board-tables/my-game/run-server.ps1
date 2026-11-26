$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'scripts/run-helpers.ps1')
Assert-PowerShellVersion
Set-Location $PSScriptRoot

if ($args.Count -ne 0) { throw 'Usage: ./run-server.ps1' }
Assert-NoRunningServer
Build-Local
$java = Get-LocalJava
& $java --enable-native-access=ALL-UNNAMED '-Dstdout.encoding=UTF-8' '-Dstderr.encoding=UTF-8' -cp "$e2eClasspath" MyGame.e2e.ServerHost
exit $LASTEXITCODE

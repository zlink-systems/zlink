$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'scripts/run-helpers.ps1')
Initialize-Console
Push-Location $PSScriptRoot
try {
    if ($args.Count -ne 0) { throw 'Usage: ./run-server.ps1' }
    if (Test-RunningServer) {
        throw 'A server is already running. Stop it with Ctrl+C in its terminal before building.'
    }
    Write-Host 'Preparing: building the server (the first run downloads Gradle and dependencies and can take a few minutes)...'
    Build-Local
    Write-Host 'Build complete. Starting servers...'
    $java = Get-LocalJava
    & $java @javaFlags -cp "$e2eClasspath" MyGame.e2e.ServerHost
    exit $LASTEXITCODE
} finally { Pop-Location }

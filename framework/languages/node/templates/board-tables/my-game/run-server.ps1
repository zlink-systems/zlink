param([switch]$Trace)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'scripts/run-helpers.ps1')
Assert-PowerShellVersion
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force '.logs' | Out-Null
$buildLog = Join-Path $PSScriptRoot '.logs/build.log'
$code = Invoke-Logged -Log $buildLog -Command npm -Arguments @('ci')
if ($code -eq 0) { $code = Invoke-Logged -Log $buildLog -Append -Command npm -Arguments @('run', 'build') }
if ($code -ne 0) {
    Get-Content $buildLog -Tail 20 | Write-Host
    exit 1
}
$hostArguments = @()
if ($Trace) { $hostArguments += '--trace' }
& node dist/e2e/server-host.js @hostArguments
exit $LASTEXITCODE

param([string]$Scenario)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
& npm install
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& npm run build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
# The runner starts the separate match, Session, and HTTP lobby entrypoints.
$arguments = @('dist/e2e/runner.js')
if ($Scenario) { $arguments += @('--scenario', $Scenario) }
& node @arguments
exit $LASTEXITCODE

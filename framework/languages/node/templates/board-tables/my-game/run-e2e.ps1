param([string]$Scenario)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
& npm ci
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& npm run build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
# The runner selects server/session/main.js and server/table/main.js from declared node roles.
$arguments = @('dist/e2e/runner.js')
if ($Scenario) { $arguments += @('--scenario', $Scenario) }
& node @arguments
exit $LASTEXITCODE

param(
  [Parameter(Position = 0)]
  [string]$Scenario = "",
  [switch]$B8Child
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if ($Scenario -eq "ZW-B8" -or $B8Child) {
  $env:ZLINK_ZONEWORLD_LANE = "b8"
}
node (Join-Path $scriptDir "../run-sample.mjs") (Join-Path $scriptDir "Runner/sample-runner.mjs")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

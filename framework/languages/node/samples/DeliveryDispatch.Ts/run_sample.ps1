$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$env:DELIVERYDISPATCH_WAIT_INTERVAL_MS = "100"
$env:DELIVERYDISPATCH_WAIT_ATTEMPTS = "300"
node (Join-Path $scriptDir "../run-sample.mjs") (Join-Path $scriptDir "Runner/sample-runner.mjs")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

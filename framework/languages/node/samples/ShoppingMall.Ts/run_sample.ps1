$ErrorActionPreference = "Stop"
$env:SHOPPINGMALL_WAIT_ATTEMPTS = "300"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
node (Join-Path $scriptDir "../run-sample.mjs") (Join-Path $scriptDir "Runner/sample-runner.mjs")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

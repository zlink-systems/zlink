$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location (Join-Path $scriptDir "../..")
try {
    npx tsc -b tsconfig.build.json --force
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Pop-Location
}
node (Join-Path $scriptDir "../run-sample.mjs") (Join-Path $scriptDir "Runner/sample-runner.mjs")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

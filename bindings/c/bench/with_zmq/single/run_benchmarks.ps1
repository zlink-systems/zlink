$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
& (Join-Path $scriptDir "..\run_benchmarks.ps1") @args
exit $LASTEXITCODE

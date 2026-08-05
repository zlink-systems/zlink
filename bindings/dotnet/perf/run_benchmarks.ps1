$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Runner = Join-Path $ScriptDir "single\run_benchmarks.ps1"
& $Runner @Args
exit $LASTEXITCODE

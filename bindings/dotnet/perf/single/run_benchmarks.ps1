$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ShellRunner = Join-Path $ScriptDir "run_benchmarks.sh"

if (Get-Command bash -ErrorAction SilentlyContinue) {
    & bash $ShellRunner @Args
    exit $LASTEXITCODE
}

Write-Error "bash is required to run bindings/dotnet/perf single benchmarks."
exit 1

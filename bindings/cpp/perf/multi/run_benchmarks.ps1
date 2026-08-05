param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ArgsList
)
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
& (Join-Path $ScriptDir "../run_benchmarks_multi.ps1") @ArgsList
exit $LASTEXITCODE

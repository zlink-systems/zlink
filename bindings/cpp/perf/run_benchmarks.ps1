param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ArgsList
)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Runner = Join-Path $ScriptDir "run_policy_bench.py"

$PassArgs = @()
for ($i = 0; $i -lt $ArgsList.Count; $i++) {
    $arg = $ArgsList[$i]
    if ($arg -eq "--duration" -and ($i + 1) -lt $ArgsList.Count) {
        $env:PERF_SINGLE_DURATION_SECONDS = $ArgsList[$i + 1]
        $i++
        continue
    }
    $PassArgs += $arg
}

& python $Runner --binding cpp --suite single @PassArgs
exit $LASTEXITCODE

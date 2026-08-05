param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ArgsList
)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Runner = Join-Path $ScriptDir "run_policy_bench.py"

$PassArgs = @()
for ($i = 0; $i -lt $ArgsList.Count; $i++) {
    $arg = $ArgsList[$i]
    switch ($arg) {
        "--clients" { $PassArgs += "--multi-clients"; $PassArgs += $ArgsList[$i + 1]; $i++; continue }
        "--duration" { $PassArgs += "--multi-duration-seconds"; $PassArgs += $ArgsList[$i + 1]; $i++; continue }
        "--hwm" { $PassArgs += "--multi-hwm"; $PassArgs += $ArgsList[$i + 1]; $i++; continue }
        default { $PassArgs += $arg }
    }
}

& python $Runner --binding cpp --suite multi @PassArgs
exit $LASTEXITCODE

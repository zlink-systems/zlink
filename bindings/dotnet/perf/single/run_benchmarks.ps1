$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ShellRunner = Join-Path $ScriptDir "run_benchmarks.sh"
$NativeBashCandidates = @(
    (Join-Path ${env:ProgramFiles} "Git\bin\bash.exe"),
    (Join-Path ${env:LOCALAPPDATA} "Programs\Git\bin\bash.exe")
)
$NativeBash = $NativeBashCandidates |
    Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -First 1
$BashRunner = "/$($ShellRunner.Substring(0, 1).ToLowerInvariant())$($ShellRunner.Substring(2).Replace('\\', '/'))"

if ($NativeBash) {
    & $NativeBash $BashRunner @Args
    exit $LASTEXITCODE
}

Write-Error "Native Git Bash is required to run bindings/dotnet/perf single benchmarks."
exit 1

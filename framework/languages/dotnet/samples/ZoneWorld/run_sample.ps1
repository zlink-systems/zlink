param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RunnerArguments
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$bash = Get-Command bash -ErrorAction SilentlyContinue
if ($null -eq $bash) {
    throw "ZoneWorld requires bash because its shell runner owns the topology, browser, and cleanup checks."
}

# Keep one scenario implementation. The PowerShell entry point supplies the supported host
# surface without duplicating the process lifecycle and evidence policy in a second runner.
$shellRunner = (Resolve-Path (Join-Path $ScriptDir "run_sample.sh")).Path
& $bash.Source $shellRunner @RunnerArguments
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

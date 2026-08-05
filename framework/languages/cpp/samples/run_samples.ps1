$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Bash = if ($env:ZLINK_CPP_BASH) { $env:ZLINK_CPP_BASH } else { "bash" }
$SampleRunners = @(
    "TicTacToe/run_sample.sh",
    "Bingo/run_sample.sh",
    "DeliveryDispatch/run_sample.sh",
    "SupportChat/run_sample.sh",
    "GameQuest/run_sample.sh",
    "ShoppingMall/run_sample.sh"
)

foreach ($Runner in $SampleRunners) {
    $RunnerPath = Join-Path $ScriptDir $Runner
    if (-not (Test-Path $RunnerPath)) {
        throw "Missing C++ sample runner: $RunnerPath"
    }
    & $Bash $RunnerPath
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

Write-Host "sample all result=passed"

param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Samples
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$defaultSamples = @(
    "TicTacToe.Ts",
    "Bingo.Ts",
    "DeliveryDispatch.Ts",
    "SupportChat.Ts",
    "GameQuest.Ts",
    "ShoppingMall.Ts",
    "ZoneWorld"
)
$selectedSamples = if ($Samples.Count -eq 0) { $defaultSamples } else { $Samples }

foreach ($sample in $selectedSamples) {
    $runner = Join-Path $scriptDir "$sample/run_sample.ps1"
    if (-not (Test-Path $runner)) {
        throw "Unknown Node sample '$sample'."
    }
    Write-Output "sample $sample start"
    & $runner
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Output "sample $sample completed"
}

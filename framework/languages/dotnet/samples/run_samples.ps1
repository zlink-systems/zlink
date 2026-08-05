$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

$knownSamples = @("TicTacToe", "Bingo", "SupportChat", "ShoppingMall", "DeliveryDispatch", "GameQuest", "ZoneWorld")
$selected = if ($args.Count -gt 0) { @($args) } else { $knownSamples }
foreach ($sample in $selected) {
    if ($sample -notin $knownSamples) {
        throw "Unknown .NET sample '$sample'."
    }
    & (Join-Path $ScriptDir "$sample/run_sample.ps1")
    if ($LASTEXITCODE -ne 0) {
        throw "$sample sample runner failed with exit code $LASTEXITCODE."
    }
}

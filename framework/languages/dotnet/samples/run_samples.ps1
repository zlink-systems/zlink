param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Samples,
    [string]$LocalPackageRoot = ''
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

$knownSamples = @("TicTacToe", "Bingo", "SupportChat", "ShoppingMall", "DeliveryDispatch", "GameQuest", "ZoneWorld")
$selected = if ($Samples -and $Samples.Count -gt 0) { @($Samples) } else { $knownSamples }
$previousPackageRoot = $env:ZLINK_LOCAL_PACKAGE_ROOT
try {
    if ($LocalPackageRoot) {
        $env:ZLINK_LOCAL_PACKAGE_ROOT = (Resolve-Path -LiteralPath $LocalPackageRoot).Path
    }
    foreach ($sample in $selected) {
        if ($sample -notin $knownSamples) {
            throw "Unknown .NET sample '$sample'."
        }
        & (Join-Path $ScriptDir "$sample/run_sample.ps1")
        if ($LASTEXITCODE -ne 0) {
            throw "$sample sample runner failed with exit code $LASTEXITCODE."
        }
    }
} finally {
    $env:ZLINK_LOCAL_PACKAGE_ROOT = $previousPackageRoot
}

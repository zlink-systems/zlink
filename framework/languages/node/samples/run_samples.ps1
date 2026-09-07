param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Samples,
    [string]$LocalPackageRoot = "",
    [switch]$SkipFrameworkBuild
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
$selectedSamples = if ($null -eq $Samples -or $Samples.Count -eq 0) { $defaultSamples } else { $Samples }

if (-not $SkipFrameworkBuild) {
    $buildArguments = @{ SkipSamples = $true }
    if (-not [string]::IsNullOrWhiteSpace($LocalPackageRoot)) {
        $buildArguments.LocalPackageRoot = $LocalPackageRoot
    }
    & (Join-Path $scriptDir "../build-windows.ps1") @buildArguments
    if (-not $?) { throw "Node Framework Windows build failed." }
}

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

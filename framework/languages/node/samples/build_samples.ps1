param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Samples,
    [string]$LocalPackageRoot = "",
    [switch]$SkipFrameworkBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$nodeRoot = Split-Path -Parent $scriptDir
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
    & (Join-Path $nodeRoot "build-windows.ps1") @buildArguments
    if (-not $?) { throw "Node Framework Windows build failed." }
}

foreach ($sample in $selectedSamples) {
    $sampleRoot = Join-Path $scriptDir $sample
    if (-not (Test-Path -LiteralPath (Join-Path $sampleRoot "package.json") -PathType Leaf)) {
        throw "Unknown Node sample '$sample'."
    }
    Write-Output "sample $sample build start"
    Push-Location $sampleRoot
    try {
        & npm.cmd run build
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    } finally {
        Pop-Location
    }
    Write-Output "sample $sample build completed"
}

if ($selectedSamples -contains "ZoneWorld") {
    $sharedBrowserRoot = Join-Path $nodeRoot "../shared_sample/zoneworld/client"
    Write-Output "sample ZoneWorld shared browser build start"
    Push-Location $sharedBrowserRoot
    try {
        & npm.cmd ci --ignore-scripts --no-audit --no-fund
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        & npm.cmd run build
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        $previousBrowserPath = $env:PLAYWRIGHT_BROWSERS_PATH
        try {
            $env:PLAYWRIGHT_BROWSERS_PATH = Join-Path $sharedBrowserRoot ".cache/ms-playwright"
            & node.exe (Join-Path $sharedBrowserRoot "node_modules/playwright/cli.js") install chromium
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        } finally {
            $env:PLAYWRIGHT_BROWSERS_PATH = $previousBrowserPath
        }
    } finally {
        Pop-Location
    }
    Write-Output "sample ZoneWorld shared browser build completed"
}

Write-Output "Node sample Windows builds passed."

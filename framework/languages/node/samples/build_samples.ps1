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

#  Positive, existence-checked marker for "this is the repository", not directory shape: a
#  examples mirror repository (zlink-node-examples) keeps the same samples/<Sample> layout,
#  so shape alone cannot tell the two apart (#655). Only the repository's node workspace root
#  carries this package.json with this name.
$nodeWorkspaceManifestPath = Join-Path $nodeRoot "package.json"
$repositoryMode = (Test-Path -LiteralPath $nodeWorkspaceManifestPath -PathType Leaf) `
    -and ((Get-Content -LiteralPath $nodeWorkspaceManifestPath -Raw | ConvertFrom-Json).name -eq "@zlink-systems/node-framework-workspace")

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

if ($repositoryMode) {
    if (-not $SkipFrameworkBuild) {
        $buildArguments = @{ SkipSamples = $true }
        if (-not [string]::IsNullOrWhiteSpace($LocalPackageRoot)) {
            $buildArguments.LocalPackageRoot = $LocalPackageRoot
        }
        & (Join-Path $nodeRoot "build-windows.ps1") @buildArguments
        if (-not $?) { throw "Node Framework Windows build failed." }
    }
} else {
    #  Standalone samples package: there is no repository framework build to run first: each
    #  sample resolves @zlink-systems/* from the registry at its pinned version once
    #  `npm install` has run (see samples/README.md).
    Write-Output "Standalone samples package: skipping the repository framework build."
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
    if ($repositoryMode) {
        $sharedBrowserRoot = Join-Path $nodeRoot "../shared_sample/zoneworld/client"
        Write-Output "sample ZoneWorld shared browser build start"
        Push-Location $sharedBrowserRoot
        try {
            $previousBrowserPath = $env:PLAYWRIGHT_BROWSERS_PATH
            try {
                $env:PLAYWRIGHT_BROWSERS_PATH = Join-Path $sharedBrowserRoot ".cache/ms-playwright"
                & npm.cmd run prepare:browser
                if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
            } finally {
                $env:PLAYWRIGHT_BROWSERS_PATH = $previousBrowserPath
            }
            & npm.cmd run build
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        } finally {
            Pop-Location
        }
        Write-Output "sample ZoneWorld shared browser build completed"
    } else {
        #  Outside the repository, ZoneWorld's browser UI is the self-contained
        #  ZoneWorld/Browser directory: it is built from ZoneWorld's own node_modules the
        #  first time the sample runs (Runner/sample-runner.mjs), not pre-built here. It only
        #  needs its own Chromium install.
        Write-Output "Standalone samples package: ZoneWorld's browser UI (ZoneWorld/Browser) builds automatically when the sample runs. Run 'npm run browser:install' inside ZoneWorld to install Chromium first."
    }
}

Write-Output "Node sample Windows builds passed."

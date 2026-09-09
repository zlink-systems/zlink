param(
    [Parameter(Mandatory = $true)][ValidateSet(
        "Bingo", "DeliveryDispatch", "GameQuest", "ShoppingMall",
        "SupportChat", "TicTacToe", "ZoneWorld")][string]$SampleName,
    [Parameter(Mandatory = $true)][string]$Destination
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (& git -C $scriptDir rev-parse --show-toplevel).Trim()
if ($LASTEXITCODE -ne 0) {
    throw "Could not resolve the repository root."
}
$samplesRoot = Join-Path $repoRoot "framework/languages/dotnet/samples"

function Copy-SourceTree {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Target
    )

    $sourcePath = (Resolve-Path $Source).Path
    Get-ChildItem -File -Recurse -Path $sourcePath |
        Where-Object {
            $relative = [System.IO.Path]::GetRelativePath($sourcePath, $_.FullName)
            $segments = $relative -split '[\\/]'
            $segments -notcontains "bin" -and $segments -notcontains "obj"
        } |
        ForEach-Object {
            $relative = [System.IO.Path]::GetRelativePath($sourcePath, $_.FullName)
            $targetFile = Join-Path $Target $relative
            $targetDirectory = Split-Path -Parent $targetFile
            New-Item -ItemType Directory -Force -Path $targetDirectory | Out-Null
            Copy-Item -Path $_.FullName -Destination $targetFile
        }
}

if (Test-Path $Destination) {
    $existing = @(Get-ChildItem -Force -Path $Destination | Select-Object -First 1)
    if ($existing.Count -ne 0) {
        throw "Destination must not exist or must be empty: $Destination"
    }
}
else {
    New-Item -ItemType Directory -Path $Destination | Out-Null
}

foreach ($file in @(
    "Directory.Build.props",
    "Directory.Packages.props",
    "nuget.config",
    "redis-common.sh",
    "sample_runner.ps1")) {
    Copy-Item -Path (Join-Path $samplesRoot $file) -Destination $Destination
}
Copy-SourceTree -Source (Join-Path $samplesRoot "Common") -Target (Join-Path $Destination "Common")
Copy-SourceTree -Source (Join-Path $samplesRoot $SampleName) -Target (Join-Path $Destination $SampleName)

$preparedPath = (Resolve-Path $Destination).Path
Write-Host "Prepared $SampleName at $preparedPath"

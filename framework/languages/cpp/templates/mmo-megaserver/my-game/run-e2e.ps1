param([string]$Scenario)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    $arguments = @('--root', $PSScriptRoot)
    if ($Scenario) { $arguments += @('--scenario', $Scenario) }
    & ./build/bin/my-game-e2e.exe @arguments
    exit $LASTEXITCODE
} finally { Pop-Location }

param(
    [switch]$NoBuild,
    [string[]]$Scenario,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$RunnerArguments = @()
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 or later is required.' }
$root = $PSScriptRoot
New-Item -ItemType Directory -Force (Join-Path $root '.logs') | Out-Null
if (-not $NoBuild) {
    & dotnet build (Join-Path $root 'server/MyGame.sln') --nologo --verbosity minimal *> (Join-Path $root '.logs/build.log')
    if ($LASTEXITCODE -ne 0) {
        Get-Content (Join-Path $root '.logs/build.log') -Tail 20 | Write-Error
        exit 1
    }
}
if ($Scenario) { $RunnerArguments = @('--scenario', ($Scenario -join ',')) + $RunnerArguments }
& dotnet (Join-Path $root 'e2e/MyGame.E2E/bin/Debug/net8.0/MyGame.E2E.dll') --root $root @RunnerArguments
exit $LASTEXITCODE

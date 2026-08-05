param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $Languages
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (git -C $ScriptDir rev-parse --show-toplevel).Trim()

if ($env:ZLINK_LOCAL_PACKAGE_ROOT) {
    $ArtifactRoot = $env:ZLINK_LOCAL_PACKAGE_ROOT
} else {
    $ArtifactRoot = Join-Path $RepoRoot ".artifacts/windows"
}

New-Item -ItemType Directory -Force -Path $ArtifactRoot | Out-Null

if (-not $Languages -or $Languages.Count -eq 0) {
    $Languages = @("dotnet", "java", "node", "cpp")
}

foreach ($Lang in $Languages) {
    switch ($Lang) {
        { $_ -in @("dotnet", "java", "node", "cpp") } {
            Write-Host "-- building local $Lang package into $ArtifactRoot"
            $env:ZLINK_LOCAL_PACKAGE_ROOT = $ArtifactRoot
            & (Join-Path $ScriptDir "$Lang/build-windows.ps1")
            if ($LASTEXITCODE -ne 0) {
                exit $LASTEXITCODE
            }
            break
        }
        default {
            Write-Error "Unknown language: $Lang. Usage: build-windows.ps1 [dotnet] [java] [node] [cpp]"
        }
    }
}

Write-Host "-- local packages are under $ArtifactRoot"

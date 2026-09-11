[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$LocalPackageRoot,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$FrameworkRoot = $PSScriptRoot
. (Join-Path $FrameworkRoot 'local_nuget.ps1')
$RepositoryRoot = [IO.Path]::GetFullPath((Join-Path $FrameworkRoot '../../..'))
$LocalPackageRoot = (Resolve-Path -LiteralPath $LocalPackageRoot).Path
$bindingVersionFile = Join-Path $RepositoryRoot 'bindings/dotnet/VERSION'
$frameworkVersionFile = Join-Path $FrameworkRoot 'VERSION'
$bindingVersion = (Select-String -LiteralPath $bindingVersionFile -Pattern '^ZLINK_BINDING_VERSION=(.+)$').Matches.Groups[1].Value
$httpVersion = (Select-String -LiteralPath $frameworkVersionFile -Pattern '^ZLINK_FRAMEWORK_VERSION=(.+)$').Matches.Groups[1].Value
if ([string]::IsNullOrWhiteSpace($bindingVersion) -or [string]::IsNullOrWhiteSpace($httpVersion)) {
    throw "Unable to evaluate binding/framework VERSION files for local package validation."
}
foreach ($package in @("Zlink.$bindingVersion.nupkg", "Zlink.HttpClient.$httpVersion.nupkg")) {
    if (-not (Test-Path -LiteralPath (Join-Path $LocalPackageRoot "nuget/$package"))) {
        throw "Missing local package: $package in $LocalPackageRoot/nuget"
    }
}
$logRoot = Join-Path $RepositoryRoot ('.artifacts/windows/logs/dotnet-build-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $logRoot -Force | Out-Null
$projects = @(Get-ChildItem -LiteralPath (Join-Path $FrameworkRoot 'src') -Recurse -Filter '*.csproj' | Sort-Object FullName)
$samples = @('TicTacToe', 'Bingo', 'DeliveryDispatch', 'SupportChat', 'GameQuest', 'ShoppingMall', 'ZoneWorld')
$targets = @($projects | ForEach-Object { $_.FullName })
$targets += @($samples | ForEach-Object { Join-Path $FrameworkRoot "samples/$_/$_.sln" })
foreach ($target in $targets) {
    $name = [IO.Path]::GetFileNameWithoutExtension($target)
    $log = Join-Path $logRoot "$name.log"
    Write-Host "Building $name ($Configuration)"
    try {
        Invoke-ZlinkDotnetBuild -Project $target -LocalPackageRoot $LocalPackageRoot -Configuration $Configuration *> $log
    } catch {
        Get-Content -LiteralPath $log -Tail 60
        throw "Build failed: $name. Log: $log. $($_.Exception.Message)"
    }
    Write-Host "PASS $name"
}
Write-Host "PASS .NET Framework projects and all 7 samples. Logs: $logRoot"

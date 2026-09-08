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
$RepositoryRoot = [IO.Path]::GetFullPath((Join-Path $FrameworkRoot '../../..'))
$LocalPackageRoot = (Resolve-Path -LiteralPath $LocalPackageRoot).Path
[xml]$versions = Get-Content -LiteralPath (Join-Path $FrameworkRoot 'Directory.Packages.props')
$bindingVersion = $versions.SelectSingleNode('//ZLinkBindingsPackageVersion').InnerText
$httpVersion = $versions.SelectSingleNode('//ZLinkHttpClientPackageVersion').InnerText
foreach ($package in @("Systems.Zlink.$bindingVersion.nupkg", "Systems.Zlink.HttpClient.$httpVersion.nupkg")) {
    if (-not (Test-Path -LiteralPath (Join-Path $LocalPackageRoot "nuget/$package"))) {
        throw "Missing local package: $package in $LocalPackageRoot/nuget"
    }
}
$previousPackageRoot = $env:ZLINK_LOCAL_PACKAGE_ROOT
$previousNugetPackages = $env:NUGET_PACKAGES
$env:ZLINK_LOCAL_PACKAGE_ROOT = $LocalPackageRoot
$env:NUGET_PACKAGES = Join-Path $LocalPackageRoot 'dotnet/framework-build-packages'
$logRoot = Join-Path $RepositoryRoot ('.artifacts/windows/logs/dotnet-build-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $logRoot -Force | Out-Null
try {
    $projects = @(Get-ChildItem -LiteralPath (Join-Path $FrameworkRoot 'src') -Recurse -Filter '*.csproj' | Sort-Object FullName)
    $samples = @('TicTacToe', 'Bingo', 'DeliveryDispatch', 'SupportChat', 'GameQuest', 'ShoppingMall', 'ZoneWorld')
    $targets = @($projects | ForEach-Object { $_.FullName })
    $targets += @($samples | ForEach-Object { Join-Path $FrameworkRoot "samples/$_/$_.sln" })
    foreach ($target in $targets) {
        $name = [IO.Path]::GetFileNameWithoutExtension($target)
        $log = Join-Path $logRoot "$name.log"
        Write-Host "Building $name ($Configuration)"
        & dotnet build $target --configuration $Configuration --maxcpucount:1 --nologo --verbosity minimal *> $log
        if ($LASTEXITCODE -ne 0) {
            Get-Content -LiteralPath $log -Tail 60
            throw "Build failed: $name. Log: $log"
        }
        Write-Host "PASS $name"
    }
    Write-Host "PASS .NET Framework projects and all 7 samples. Logs: $logRoot"
} finally {
    $env:ZLINK_LOCAL_PACKAGE_ROOT = $previousPackageRoot
    $env:NUGET_PACKAGES = $previousNugetPackages
}

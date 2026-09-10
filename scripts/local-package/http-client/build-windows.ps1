[CmdletBinding()]
param(
    [string]$RepositoryRoot = "",
    [string]$LocalPackageRoot = "",
    [ValidateSet("dotnet", "java", "node")]
    [string[]]$Language = @("dotnet", "java", "node"),
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
} else {
    $RepositoryRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
}
if ([string]::IsNullOrWhiteSpace($LocalPackageRoot)) {
    $LocalPackageRoot = Join-Path $RepositoryRoot ".artifacts\windows"
}
$LocalPackageRoot = [IO.Path]::GetFullPath($LocalPackageRoot)
New-Item -ItemType Directory -Force -Path $LocalPackageRoot | Out-Null

function Get-PackageVersion([string]$RelativePath, [string]$Key) {
    $path = Join-Path $RepositoryRoot $RelativePath
    $match = Select-String -LiteralPath $path -Pattern "^${Key}=([0-9]+\.[0-9]+\.[0-9]+)$"
    if (-not $match) {
        throw "Unable to read $Key from $path"
    }
    return $match.Matches[0].Groups[1].Value
}

function Invoke-Checked(
    [string]$Executable,
    [string[]]$Arguments,
    [string]$WorkingDirectory
) {
    Push-Location $WorkingDirectory
    $previousErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        & $Executable @Arguments
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
        Pop-Location
    }
    if ($exitCode -ne 0) {
        throw ("Command failed with exit code {0}: {1} {2}" -f $exitCode, $Executable, ($Arguments -join ' '))
    }
}

$previousPackageRoot = $env:ZLINK_LOCAL_PACKAGE_ROOT
try {
    $env:ZLINK_LOCAL_PACKAGE_ROOT = $LocalPackageRoot
    foreach ($item in $Language) {
        $frameworkVersion = Get-PackageVersion "framework\languages\$item\VERSION" "ZLINK_FRAMEWORK_VERSION"
        switch ($item) {
            "dotnet" {
                $out = Join-Path $LocalPackageRoot "nuget"
                New-Item -ItemType Directory -Force -Path $out | Out-Null
                $root = Join-Path $RepositoryRoot "framework\languages\dotnet"
                foreach ($project in @(
                    "src\Systems.Zlink.Stream.Connector\Systems.Zlink.Stream.Connector.csproj",
                    "src\Zlink.Framework.Contracts\Zlink.Framework.Contracts.csproj",
                    "src\Zlink.HttpClient\Zlink.HttpClient.csproj"
                )) {
                    Invoke-Checked dotnet @("pack", (Join-Path $root $project), "-c", $Configuration, "-o", $out) $root
                }
                foreach ($name in @("Zlink.Stream.Connector", "Zlink.Framework.Contracts", "Zlink.HttpClient")) {
                    $package = Join-Path $out "$name.$frameworkVersion.nupkg"
                    if (-not (Test-Path -LiteralPath $package -PathType Leaf)) {
                        throw "Missing .NET Framework package: $package"
                    }
                }
            }
            "java" {
                $out = Join-Path $LocalPackageRoot "maven"
                New-Item -ItemType Directory -Force -Path $out | Out-Null
                $root = Join-Path $RepositoryRoot "framework\languages\java"
                $previousRepository = $env:MAVEN_REPOSITORY_URL
                try {
                    $env:MAVEN_REPOSITORY_URL = ([Uri]$out).AbsoluteUri
                    Invoke-Checked (Join-Path $root "gradlew.bat") @("--no-daemon", ":zlink-http-client:publish", ":zlink-http-client-kotlin:publish") $root
                } finally {
                    $env:MAVEN_REPOSITORY_URL = $previousRepository
                }
                $module = Join-Path $out "systems\zlink\zlink-http-client\$frameworkVersion"
                if (-not (Test-Path -LiteralPath $module -PathType Container)) {
                    throw "Missing Java HTTP client package: $module"
                }
            }
            "node" {
                $out = Join-Path $LocalPackageRoot "npm"
                New-Item -ItemType Directory -Force -Path $out | Out-Null
                $root = Join-Path $RepositoryRoot "framework\languages\node"
                $bindingVersion = Get-PackageVersion "bindings\node\VERSION" "ZLINK_BINDING_VERSION"
                $bindingPackage = Join-Path $out "zlink-systems-zlink-$bindingVersion.tgz"
                if (-not (Test-Path -LiteralPath $bindingPackage -PathType Leaf)) {
                    throw "Node binding package is required first: $bindingPackage"
                }
                $npm = (Get-Command npm.cmd -ErrorAction Stop).Source
                $node = (Get-Command node.exe -ErrorAction Stop).Source
                Invoke-Checked $npm @("install", "--no-save", "--no-package-lock", "--ignore-scripts", "--no-audit", "--no-fund", $bindingPackage, (Join-Path $root "packages\http-client")) $root
                Invoke-Checked $node @("node_modules\typescript\bin\tsc", "-b", "packages\http-client") $root
                Invoke-Checked $npm @("pack", "--pack-destination", $out, ".\packages\http-client") $root
                $package = Join-Path $out "zlink-systems-http-client-$frameworkVersion.tgz"
                if (-not (Test-Path -LiteralPath $package -PathType Leaf)) {
                    throw "Missing Node HTTP client package: $package"
                }
            }
        }
        Write-Output "$item HTTP client local package result=passed version=$frameworkVersion"
    }
} finally {
    $env:ZLINK_LOCAL_PACKAGE_ROOT = $previousPackageRoot
}

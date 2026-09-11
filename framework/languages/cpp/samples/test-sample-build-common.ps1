$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. "$PSScriptRoot/sample-build-common.ps1"

$fixture = Join-Path ([IO.Path]::GetTempPath()) "zlink-sample-resolver-$([Guid]::NewGuid().ToString('N'))"
$oldBuildDir = $env:ZLINK_CPP_BUILD_DIR
$oldConfiguration = $env:ZLINK_CPP_BUILD_CONFIGURATION
$checks = 0
function New-TestBuild([string]$RelativePath, [string]$Configuration, [string[]]$Binaries) {
    $path = Join-Path $fixture $RelativePath
    New-Item -ItemType Directory -Force $path | Out-Null
    Set-Content -LiteralPath (Join-Path $path 'CMakeCache.txt') -Value "CMAKE_BUILD_TYPE:STRING=$Configuration" -Encoding ASCII
    foreach ($binary in $Binaries) {
        $file = Join-Path $path $binary
        New-Item -ItemType Directory -Force (Split-Path $file) | Out-Null
        New-Item -ItemType File $file | Out-Null
    }
    return $path
}
function Assert-Equal($Actual, $Expected, [string]$Description) {
    if ($Actual -ne $Expected) { throw "$Description`: expected '$Expected', got '$Actual'" }
    $script:checks++
}
try {
    $env:ZLINK_CPP_BUILD_DIR = $null
    $env:ZLINK_CPP_BUILD_CONFIGURATION = $null
    $sample = Join-Path $fixture 'samples/Example'
    $shared = New-TestBuild 'build' 'Release' @('server.exe', 'client.exe')
    $ninja = New-TestBuild 'samples/Example/build/windows-ninja' 'Debug' @('server.exe', 'client.exe')
    $args = @{ SampleDir = $sample; CppRoot = $fixture; RequiredBinaries = @('server', 'client') }
    $result = Resolve-ZlinkCppSampleBuild @args
    Assert-Equal $result.BuildDir $ninja 'Standalone preset wins over shared build'
    Assert-Equal $result.Configuration 'Debug' 'Ninja cache configuration'
    Assert-Equal (Get-ZlinkCppSampleBinary $result 'server') (Join-Path $ninja 'server.exe') 'Windows binary suffix'

    $env:ZLINK_CPP_BUILD_DIR = $shared
    Assert-Equal (Resolve-ZlinkCppSampleBuild @args).BuildDir $shared 'Explicit build wins'
    $vs = New-TestBuild 'vs' '' @('Debug/server.exe', 'Debug/client.exe', 'Release/server.exe', 'Release/client.exe')
    $env:ZLINK_CPP_BUILD_DIR = $vs
    Assert-Equal (Resolve-ZlinkCppSampleBuild @args).Configuration 'Debug' 'Multi-config Debug default'
    $env:ZLINK_CPP_BUILD_CONFIGURATION = 'Release'
    Assert-Equal (Resolve-ZlinkCppSampleBuild @args).Configuration 'Release' 'Explicit configuration wins'

    $release = New-TestBuild 'release-only' '' @('Release/server.exe', 'Release/client.exe')
    $env:ZLINK_CPP_BUILD_DIR = $release
    $env:ZLINK_CPP_BUILD_CONFIGURATION = $null
    Assert-Equal (Resolve-ZlinkCppSampleBuild @args).Configuration 'Release' 'Release fallback'
    $partial = New-TestBuild 'partial' 'Debug' @('server.exe')
    $env:ZLINK_CPP_BUILD_DIR = $partial
    $rejected = $false
    try { Resolve-ZlinkCppSampleBuild @args | Out-Null } catch { $rejected = $_.Exception.Message -like 'Missing sample executables*' }
    Assert-Equal $rejected $true 'Partial build cannot mix binaries or use unrelated fallback'
    $env:ZLINK_CPP_BUILD_DIR = $null
    $args.SampleDir = Join-Path $fixture 'samples/Unbuilt'
    Assert-Equal (Resolve-ZlinkCppSampleBuild @args).BuildDir $shared 'Shared development fallback'
    $clean = New-TestBuild 'clean-release' '' @()
    $env:ZLINK_CPP_BUILD_DIR = $clean
    New-Item -ItemType Directory -Force (Join-Path $clean 'Release') | Out-Null
    $cleanResult = Resolve-ZlinkCppSampleBuild @args -AllowMissingBinaries
    Assert-Equal $cleanResult.Configuration 'Release' 'Clean multi-config build selection'
    $cleanMulti = New-TestBuild 'clean-multi' '' @()
    Add-Content -LiteralPath (Join-Path $cleanMulti 'CMakeCache.txt') `
        -Value 'CMAKE_CONFIGURATION_TYPES:STRING=Debug;Release;MinSizeRel;RelWithDebInfo' -Encoding ASCII
    $env:ZLINK_CPP_BUILD_DIR = $cleanMulti
    $env:ZLINK_CPP_BUILD_CONFIGURATION = 'Debug'
    $cleanMultiResult = Resolve-ZlinkCppSampleBuild @args -AllowMissingBinaries
    Assert-Equal $cleanMultiResult.BinDir (Join-Path $cleanMulti 'Debug') 'Clean multi-config binary directory'
    Assert-Equal $cleanMultiResult.Configuration 'Debug' 'Clean multi-config explicit configuration'
    Write-Host "sample-build-common: $checks checks passed"
} finally {
    $env:ZLINK_CPP_BUILD_DIR = $oldBuildDir
    $env:ZLINK_CPP_BUILD_CONFIGURATION = $oldConfiguration
    # Only the unique fixture created by this test is removed.
    if ((Split-Path $fixture -Leaf) -notlike 'zlink-sample-resolver-*') { throw 'Unsafe fixture cleanup path' }
    Remove-Item -LiteralPath $fixture -Recurse -Force
}

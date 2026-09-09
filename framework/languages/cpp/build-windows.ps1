[CmdletBinding()]
param(
    [string]$BuildDir,
    [string]$LocalPackageRoot,
    [string]$VcpkgInstalledDir,
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [ValidateRange(1, 64)]
    [int]$Parallel = 8,
    [switch]$IncludeTests,
    [switch]$IncludeE2E,
    [switch]$Install
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Invoke-ZlinkCMake {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$FailureMessage
    )
    $PreviousErrorActionPreference = $ErrorActionPreference
    try {
        # Windows PowerShell wraps legitimate CMake stderr warnings as
        # NativeCommandError records. The native exit code remains the verdict.
        $ErrorActionPreference = "Continue"
        & cmake @Arguments
        $CMakeExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $PreviousErrorActionPreference
    }
    if ($CMakeExitCode -ne 0) {
        throw "$FailureMessage with exit code $CMakeExitCode."
    }
}

$CppRoot = $PSScriptRoot
$RepositoryRoot = (Resolve-Path (Join-Path $CppRoot "../../..")).Path
$CoreVersion = (Select-String -LiteralPath (Join-Path $RepositoryRoot "VERSION") -Pattern "^LIBZLINK_VERSION=(.+)$").Matches.Groups[1].Value
$BindingVersion = (Select-String -LiteralPath (Join-Path $RepositoryRoot "BINDINGS_VERSION") -Pattern "^ZLINK_BINDINGS_VERSION=(.+)$").Matches.Groups[1].Value
$FrameworkVersion = (Select-String -LiteralPath (Join-Path $RepositoryRoot "FRAMEWORK_VERSION") -Pattern "^ZLINK_FRAMEWORK_VERSION=(.+)$").Matches.Groups[1].Value
if (@($CoreVersion, $BindingVersion, $FrameworkVersion) | Where-Object { [string]::IsNullOrWhiteSpace($_) }) {
    throw "Unable to read Core, binding, or Framework version from $RepositoryRoot"
}
$CleanPackageRoot = Join-Path $RepositoryRoot ".artifacts/cpp-clean-$BindingVersion-package"

if (-not $BuildDir) {
    $BuildDir = Join-Path $RepositoryRoot ".artifacts/windows/build/framework-cpp"
}
if (-not $LocalPackageRoot) {
    $LocalPackageRoot = if ($env:ZLINK_LOCAL_PACKAGE_ROOT) {
        $env:ZLINK_LOCAL_PACKAGE_ROOT
    } elseif (Test-Path $CleanPackageRoot) {
        $CleanPackageRoot
    } else {
        Join-Path $RepositoryRoot ".artifacts/windows"
    }
}
if (-not $VcpkgInstalledDir) {
    $VcpkgInstalledDir = if (Test-Path (Join-Path $RepositoryRoot ".artifacts/windows-vcpkg-installed")) {
        Join-Path $RepositoryRoot ".artifacts/windows-vcpkg-installed"
    } else {
        Join-Path $RepositoryRoot ".artifacts/windows/vcpkg-installed"
    }
}

$CorePrefix = Join-Path $LocalPackageRoot "install/zlink-core/$CoreVersion"
$CppPrefix = Join-Path $LocalPackageRoot "install/zlink-cpp/$BindingVersion"
foreach ($Prefix in @($CorePrefix, $CppPrefix)) {
    if (-not (Test-Path $Prefix)) {
        throw "Missing local package: $Prefix. Publish Core and the C++ binding locally first."
    }
}
if (-not (Test-Path $VcpkgInstalledDir)) {
    throw "Missing vcpkg installed tree: $VcpkgInstalledDir"
}

$CoreCMakeDir = Join-Path $CorePrefix "lib/cmake/zlink"
if (-not (Test-Path (Join-Path $CoreCMakeDir "zlinkConfig.cmake"))) {
    foreach ($RequiredPath in @("include/zlink.h", "bin/zlink.dll", "lib/zlink.lib")) {
        if (-not (Test-Path (Join-Path $CorePrefix $RequiredPath) -PathType Leaf)) {
            throw "Core release package is missing $RequiredPath`: $CorePrefix"
        }
    }
    $CoreCMakeDir = Join-Path $BuildDir "package-shims/zlink"
    New-Item -ItemType Directory -Force -Path $CoreCMakeDir | Out-Null
    $CorePrefixCMake = ([IO.Path]::GetFullPath($CorePrefix)).Replace("\", "/")
    $CoreConfig = @'
set(ZLINK_CORE_PACKAGE_PREFIX "@CORE_PREFIX@")
if(NOT TARGET libzlink)
  add_library(libzlink SHARED IMPORTED)
  set_target_properties(libzlink PROPERTIES
    IMPORTED_LOCATION "${ZLINK_CORE_PACKAGE_PREFIX}/bin/zlink.dll"
    IMPORTED_IMPLIB "${ZLINK_CORE_PACKAGE_PREFIX}/lib/zlink.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${ZLINK_CORE_PACKAGE_PREFIX}/include")
  foreach(_zlink_config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_property(TARGET libzlink APPEND PROPERTY IMPORTED_CONFIGURATIONS "${_zlink_config}")
    set_target_properties(libzlink PROPERTIES
      IMPORTED_LOCATION_${_zlink_config} "${ZLINK_CORE_PACKAGE_PREFIX}/bin/zlink.dll"
      IMPORTED_IMPLIB_${_zlink_config} "${ZLINK_CORE_PACKAGE_PREFIX}/lib/zlink.lib")
  endforeach()
endif()
set(zlink_FOUND TRUE)
'@.Replace("@CORE_PREFIX@", $CorePrefixCMake)
    $CoreConfigVersion = @"
set(PACKAGE_VERSION `"$CoreVersion`")
if(PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)
  set(PACKAGE_VERSION_EXACT TRUE)
  set(PACKAGE_VERSION_COMPATIBLE TRUE)
else()
  set(PACKAGE_VERSION_COMPATIBLE FALSE)
endif()
"@
    $Utf8NoBom = [Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText((Join-Path $CoreCMakeDir "zlinkConfig.cmake"), $CoreConfig, $Utf8NoBom)
    [IO.File]::WriteAllText((Join-Path $CoreCMakeDir "zlinkConfigVersion.cmake"), $CoreConfigVersion, $Utf8NoBom)
}

$VcpkgRoot = if ($env:VCPKG_ROOT) {
    $env:VCPKG_ROOT
} else {
    "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/vcpkg"
}
$Toolchain = Join-Path $VcpkgRoot "scripts/buildsystems/vcpkg.cmake"
if (-not (Test-Path $Toolchain)) {
    throw "vcpkg toolchain was not found at $Toolchain. Set VCPKG_ROOT."
}

$TestsEnabled = if ($IncludeTests) { "ON" } else { "OFF" }
$E2EEnabled = if ($IncludeE2E) { "ON" } else { "OFF" }
$ImportedConfigurationMaps = @("Debug", "Release", "RelWithDebInfo", "MinSizeRel") |
    Where-Object { $_ -ne $Configuration } |
    ForEach-Object { "-DCMAKE_MAP_IMPORTED_CONFIG_$($_.ToUpperInvariant())=$Configuration" }

$ConfigureArguments = @(
    "-S", $CppRoot, "-B", $BuildDir, "-G", "Visual Studio 17 2022", "-A", "x64",
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
    "-DVCPKG_INSTALLED_DIR=$VcpkgInstalledDir",
    "-DVCPKG_MANIFEST_MODE=OFF",
    "-Dzlink_DIR=$CoreCMakeDir",
    "-DCMAKE_CXX_FLAGS=/EHsc /bigobj /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00",
    "-DCMAKE_CXX_FLAGS_RELEASE=/Od /DNDEBUG",
    "-DZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT=$LocalPackageRoot",
    "-DZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION=$CoreVersion",
    "-DZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION=$BindingVersion",
    "-DZLINK_FRAMEWORK_CPP_BUILD_TESTS=$TestsEnabled",
    "-DZLINK_FRAMEWORK_CPP_BUILD_FOUNDATION_TESTS=$TestsEnabled",
    "-DZLINK_FRAMEWORK_CPP_BUILD_E2E=$E2EEnabled",
    "-DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=ON",
    "-DZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK=ON"
) + $ImportedConfigurationMaps
Invoke-ZlinkCMake -FailureMessage "C++ Framework configure failed" -Arguments $ConfigureArguments

Invoke-ZlinkCMake -FailureMessage "C++ Framework build failed" -Arguments @(
    "--build", $BuildDir, "--config", $Configuration, "--parallel", "$Parallel"
)

# Server sample targets are intentionally EXCLUDE_FROM_ALL. Build every client
# and server target explicitly so a successful default build cannot omit them.
$SampleTargets = @(
    "sample_cpp_framework_bingo_api",
    "sample_cpp_framework_bingo_matchmaking",
    "sample_cpp_framework_bingo_play",
    "sample_cpp_framework_bingo_session",
    "sample_cpp_framework_bingo_client",
    "sample_cpp_framework_tictactoe_api",
    "sample_cpp_framework_tictactoe_play",
    "sample_cpp_framework_tictactoe_client",
    "sample_cpp_framework_deliverydispatch_dispatch",
    "sample_cpp_framework_deliverydispatch_courier_actor_node",
    "sample_cpp_framework_deliverydispatch_customer_gateway",
    "sample_cpp_framework_deliverydispatch_courier_session",
    "sample_cpp_framework_deliverydispatch_tracking",
    "sample_cpp_framework_deliverydispatch_client",
    "sample_cpp_framework_gamequest_game_api",
    "sample_cpp_framework_gamequest_quest_mission",
    "sample_cpp_framework_gamequest_client",
    "sample_cpp_framework_shoppingmall_commerce_api",
    "sample_cpp_framework_shoppingmall_order_workflow",
    "sample_cpp_framework_shoppingmall_client",
    "sample_cpp_framework_supportchat_api",
    "sample_cpp_framework_supportchat_session",
    "sample_cpp_framework_supportchat_support",
    "sample_cpp_framework_supportchat_client",
    "sample_cpp_framework_zoneworld_zone_node",
    "sample_cpp_framework_zoneworld_gateway",
    "sample_cpp_framework_zoneworld_ops",
    "sample_cpp_framework_zoneworld_client"
)
$SampleBuildArguments = @(
    "--build", $BuildDir, "--config", $Configuration, "--parallel", "$Parallel", "--target"
) + $SampleTargets
Invoke-ZlinkCMake -FailureMessage "C++ Framework sample build failed" -Arguments $SampleBuildArguments

if ($Install) {
    $InstallPrefix = Join-Path $LocalPackageRoot "install/zlink-framework-cpp/$FrameworkVersion"
    Invoke-ZlinkCMake -FailureMessage "C++ Framework install failed" -Arguments @(
        "--install", $BuildDir, "--config", $Configuration, "--prefix", $InstallPrefix
    )
    Write-Host "C++ Framework install result=passed prefix=$InstallPrefix"
}

Write-Host "C++ Framework build result=passed configuration=$Configuration samples=$($SampleTargets.Count) buildDir=$BuildDir"

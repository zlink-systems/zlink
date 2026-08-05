param(
    [string] $Configuration = $env:CONFIGURATION
)

$ErrorActionPreference = "Stop"

if (-not $Configuration) {
    $Configuration = "Release"
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (git -C $ScriptDir rev-parse --show-toplevel).Trim()

if ($env:ZLINK_LOCAL_PACKAGE_ROOT) {
    $ArtifactRoot = $env:ZLINK_LOCAL_PACKAGE_ROOT
} else {
    $ArtifactRoot = Join-Path $RepoRoot ".artifacts/windows"
}

if ($env:ZLINK_CPP_PACKAGE_VERSION) {
    $PackageVersion = $env:ZLINK_CPP_PACKAGE_VERSION
} else {
    $VersionLine = Select-String -Path (Join-Path $RepoRoot "bindings/cpp/CMakeLists.txt") -Pattern '^project\(zlink_cpp VERSION ([0-9]+\.[0-9]+\.[0-9]+)' | Select-Object -First 1
    if (-not $VersionLine) {
        Write-Error "Unable to resolve zlink_cpp package version"
    }
    $PackageVersion = $VersionLine.Matches[0].Groups[1].Value
}

if ($env:ZLINK_CPP_LOCAL_BUILD_DIR) {
    $BuildDir = $env:ZLINK_CPP_LOCAL_BUILD_DIR
} else {
    $BuildDir = Join-Path $ArtifactRoot "build/bindings-cpp-$PackageVersion"
}

if ($env:ZLINK_CPP_INSTALL_PREFIX) {
    $InstallPrefix = $env:ZLINK_CPP_INSTALL_PREFIX
} else {
    $InstallPrefix = Join-Path $ArtifactRoot "install/zlink-cpp/$PackageVersion"
}

if (-not $env:ZLINK_CORE_INSTALL_PREFIX -or -not [System.IO.Path]::IsPathRooted($env:ZLINK_CORE_INSTALL_PREFIX)) {
    Write-Error "ZLINK_CORE_INSTALL_PREFIX must name an absolute installed Core 11 package prefix"
}
$CorePrefix = $env:ZLINK_CORE_INSTALL_PREFIX

cmake -S (Join-Path $RepoRoot "bindings/cpp") -B $BuildDir `
    -DCMAKE_INSTALL_PREFIX="$InstallPrefix" `
    -DZLINK_CPP_CORE_PACKAGE_PREFIX="$CorePrefix" `
    -DZLINK_CPP_BUILD_TESTS=OFF `
    -DZLINK_CPP_BUILD_SAMPLES=OFF
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$CachePath = Join-Path $BuildDir "CMakeCache.txt"
$IsMultiConfig = Select-String -Path $CachePath -Pattern "^CMAKE_CONFIGURATION_TYPES:" -Quiet

if ($IsMultiConfig) {
    cmake --build $BuildDir --config $Configuration
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    cmake --install $BuildDir --config $Configuration
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} else {
    cmake --build $BuildDir
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    cmake --install $BuildDir
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

Write-Host "-- C++ local package version: $PackageVersion"
Write-Host "-- C++ local install prefix: $InstallPrefix"

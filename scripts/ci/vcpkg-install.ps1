if ([string]::IsNullOrWhiteSpace($env:RUNNER_NAME)) {
    throw "RUNNER_NAME is required"
}
$workRoot = Join-Path $env:USERPROFILE ".cache/zlink/vcpkg-work/$env:RUNNER_NAME"
$env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $env:USERPROFILE ".cache/zlink/vcpkg-binary"
$buildtrees = Join-Path $workRoot "buildtrees"
$packages = Join-Path $workRoot "packages"
$downloads = Join-Path $workRoot "downloads"
New-Item -ItemType Directory -Force -Path $buildtrees, $packages, $downloads, $env:VCPKG_DEFAULT_BINARY_CACHE | Out-Null
if ($env:VCPKG_BINARY_SOURCES) {
    $env:VCPKG_BINARY_SOURCES += ";files,$($env:VCPKG_DEFAULT_BINARY_CACHE),readwrite"
}
& (Join-Path $env:VCPKG_ROOT "vcpkg.exe") install `
    "--x-buildtrees-root=$buildtrees" "--x-packages-root=$packages" "--downloads-root=$downloads" @args
exit $LASTEXITCODE

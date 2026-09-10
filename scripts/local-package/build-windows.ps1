param(
  [string]$CorePrefix = "",
  [string]$RepositoryRoot = "",
  [ValidateSet("cpp", "dotnet", "java", "node")]
  [string[]]$Language = @("cpp", "dotnet", "java", "node"),
  [ValidateSet("Release", "Debug")]
  [string]$Configuration = "Release",
  [string]$PythonExecutable = "",
  [switch]$SyncVersions,
  [switch]$VerifyVersions
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
  $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
} else {
  $RepositoryRoot = (Resolve-Path $RepositoryRoot).Path
}
$syncScript = Join-Path $RepositoryRoot "scripts\local-package\sync-version.py"
if ([string]::IsNullOrWhiteSpace($PythonExecutable)) {
  $pythonCommand = Get-Command python.exe, python3.exe -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($pythonCommand) {
    $PythonExecutable = $pythonCommand.Source
  } else {
    $pythonRoot = Join-Path $env:LOCALAPPDATA "Programs\Python"
    $PythonExecutable = Get-ChildItem -LiteralPath $pythonRoot -Filter python.exe -Recurse -File -ErrorAction SilentlyContinue |
      Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
  }
}
if ([string]::IsNullOrWhiteSpace($PythonExecutable) -or -not (Test-Path -LiteralPath $PythonExecutable -PathType Leaf)) {
  throw "Python 3 is required for version synchronization; pass -PythonExecutable"
}
$python = (Resolve-Path -LiteralPath $PythonExecutable).Path
if ($SyncVersions -and $VerifyVersions) {
  throw "Use only one of -SyncVersions or -VerifyVersions"
}
if ($SyncVersions) {
  & $python $syncScript --write
  if ($LASTEXITCODE -ne 0) { throw "Version synchronization failed" }
  & $python $syncScript --check
  if ($LASTEXITCODE -ne 0) { throw "Version verification failed after synchronization" }
  return
}
if ($VerifyVersions) {
  & $python $syncScript --check
  if ($LASTEXITCODE -ne 0) { throw "Version verification failed" }
  return
}
& $python $syncScript --check
if ($LASTEXITCODE -ne 0) { throw "Version verification failed" }
if ([string]::IsNullOrWhiteSpace($CorePrefix)) {
  throw "CorePrefix is required when building Windows packages"
}
$CorePrefix = (Resolve-Path $CorePrefix).Path
$artifactRoot = Join-Path $RepositoryRoot ".artifacts\windows"
$versionFile = Join-Path $RepositoryRoot "VERSION"
$coreVersion = (Select-String -LiteralPath $versionFile -Pattern "^LIBZLINK_VERSION=(.+)$").Matches.Groups[1].Value
$manifest = Join-Path $CorePrefix "share\zlink\core-package-provenance.json"

if ([string]::IsNullOrWhiteSpace($coreVersion)) {
  throw "Unable to read Core version from $RepositoryRoot"
}
if (-not (Test-Path -LiteralPath (Join-Path $CorePrefix "include\zlink.h"))) {
  throw "Core prefix is missing public headers: $CorePrefix"
}
if (-not (Test-Path -LiteralPath (Join-Path $CorePrefix "bin\zlink.dll"))) {
  throw "Core prefix is missing zlink.dll: $CorePrefix"
}
if (-not (Test-Path -LiteralPath $manifest)) {
  throw "Core prefix is missing provenance: $manifest"
}
$provenance = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
if ($provenance.package -ne "zlink-core" -or $provenance.version -ne $coreVersion -or $provenance.abiMajor -ne 0) {
  throw "Core prefix provenance does not match Core ${coreVersion}: $manifest"
}
$runtimeHash = (Get-FileHash -LiteralPath (Join-Path $CorePrefix 'bin\zlink.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
if ($provenance.runtime.sha256 -ne $runtimeHash) {
  throw "Core runtime hash does not match its provenance: $CorePrefix"
}

New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null

function Invoke-Checked([string]$FileName, [string[]]$Arguments, [string]$WorkingDirectory = $RepositoryRoot) {
  Push-Location $WorkingDirectory
  $previousErrorAction = $ErrorActionPreference
  try {
    # PowerShell 5 can surface a successful native tool's stderr as
    # NativeCommandError. Native exit status remains the authoritative result.
    $ErrorActionPreference = "Continue"
    & $FileName @Arguments
    $exitCode = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $previousErrorAction
    Pop-Location
  }
  if ($exitCode -ne 0) { throw "$FileName failed with exit code $exitCode" }
}

function Get-BindingVersion([string]$Name) {
  $path = Join-Path $RepositoryRoot "bindings\$Name\VERSION"
  $match = Select-String -LiteralPath $path -Pattern "^ZLINK_BINDING_VERSION=([0-9]+\.[0-9]+\.[0-9]+)$"
  if (-not $match) { throw "Unable to read $Name binding version from $path" }
  return $match.Matches[0].Groups[1].Value
}

foreach ($item in $Language) {
  $bindingVersion = Get-BindingVersion $item
  switch ($item) {
    "cpp" {
      $prefix = Join-Path $artifactRoot "install\zlink-cpp\$bindingVersion"
      $build = Join-Path $artifactRoot "build\bindings-cpp-$bindingVersion"
      Remove-Item -LiteralPath $prefix -Recurse -Force -ErrorAction SilentlyContinue
      Invoke-Checked cmake @(
        "-S", (Join-Path $RepositoryRoot "bindings\cpp"), "-B", $build,
        "-G", "Visual Studio 17 2022", "-A", "x64",
        "-DCMAKE_INSTALL_PREFIX=$prefix",
        "-DZLINK_CPP_CORE_PACKAGE_PREFIX=$CorePrefix",
        "-DZLINK_CPP_BUILD_TESTS=OFF", "-DZLINK_CPP_BUILD_SAMPLES=OFF"
      )
      Invoke-Checked cmake @("--build", $build, "--config", $Configuration, "--parallel")
      Invoke-Checked cmake @("--install", $build, "--config", $Configuration)
      if (-not (Test-Path -LiteralPath (Join-Path $prefix "include\zlink.hpp"))) {
        throw "C++ package headers are missing: $prefix"
      }
      if (-not (Test-Path -LiteralPath (Join-Path $prefix "lib\zlink_cpp.lib"))) {
        throw "C++ package library is missing: $prefix"
      }
    }
    "dotnet" {
      $out = Join-Path $artifactRoot "nuget"
      New-Item -ItemType Directory -Force -Path $out | Out-Null
      Invoke-Checked dotnet @(
        "pack", (Join-Path $RepositoryRoot "bindings\dotnet\src\Zlink\Zlink.csproj"),
        "-c", $Configuration, "-o", $out,
        "-p:ZLinkWindowsX64NativeRoot=$(Join-Path $CorePrefix 'bin')",
        "-p:ZLinkCoreVersion=$coreVersion",
        "-p:ZLinkCoreProvenancePath=$manifest"
      )
      $package = Join-Path $out "Zlink.$bindingVersion.nupkg"
      if (-not (Test-Path -LiteralPath $package)) { throw "NuGet package is missing: $package" }
    }
    "java" {
      $maven = Join-Path $artifactRoot "maven"
      New-Item -ItemType Directory -Force -Path $maven | Out-Null
      $runtime = Join-Path $CorePrefix "bin\zlink.dll"
      $summary = [ordered]@{
        version = $coreVersion
        prefix = $CorePrefix
        provenanceSha256 = (Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash.ToLowerInvariant()
        runtime = [ordered]@{ sha256 = (Get-FileHash -LiteralPath $runtime -Algorithm SHA256).Hash.ToLowerInvariant(); soname = "zlink.dll" }
      } | ConvertTo-Json -Compress
      $previousPrefix = $env:ZLINK_CORE_PACKAGE_PREFIX
      $previousSummary = $env:ZLINK_CORE_PACKAGE_SUMMARY
      $previousCoreVersion = $env:ZLINK_CORE_VERSION
      $previousRepository = $env:MAVEN_REPOSITORY_URL
      try {
        $env:ZLINK_CORE_PACKAGE_PREFIX = $CorePrefix
        $env:ZLINK_CORE_PACKAGE_SUMMARY = $summary
        $env:ZLINK_CORE_VERSION = $coreVersion
        $env:MAVEN_REPOSITORY_URL = ([Uri]$maven).AbsoluteUri
        Invoke-Checked (Join-Path $RepositoryRoot "bindings\java\gradlew.bat") @(
          "--no-daemon",
          "--init-script", (Join-Path $PSScriptRoot "java\windows-package.init.gradle"),
          "clean", "publishMavenJavaPublicationToReleaseRepoRepository"
        ) (Join-Path $RepositoryRoot "bindings\java")
      } finally {
        $env:ZLINK_CORE_PACKAGE_PREFIX = $previousPrefix
        $env:ZLINK_CORE_PACKAGE_SUMMARY = $previousSummary
        $env:ZLINK_CORE_VERSION = $previousCoreVersion
        $env:MAVEN_REPOSITORY_URL = $previousRepository
      }
      $jar = Join-Path $maven "systems\zlink\zlink\$bindingVersion\zlink-$bindingVersion.jar"
      if (-not (Test-Path -LiteralPath $jar)) { throw "Java binding package is missing: $jar" }
    }
    "node" {
      $nodeRoot = Join-Path $RepositoryRoot "bindings\node"
      $npm = (Get-Command npm.cmd -ErrorAction Stop).Source
      $npx = (Get-Command npx.cmd -ErrorAction Stop).Source
      $out = Join-Path $artifactRoot "npm"
      New-Item -ItemType Directory -Force -Path $out | Out-Null
      $previousPrefix = $env:ZLINK_CORE_INSTALL_PREFIX
      $previousSource = $env:ZLINK_CORE_SOURCE
      try {
        $env:ZLINK_CORE_INSTALL_PREFIX = $CorePrefix
        $env:ZLINK_CORE_SOURCE = "release"
        Remove-Item -LiteralPath (Join-Path $nodeRoot "prebuilds") -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath (Join-Path $nodeRoot "provenance") -Recurse -Force -ErrorAction SilentlyContinue
        Invoke-Checked $npm @("ci", "--ignore-scripts") $nodeRoot
        Invoke-Checked $npm @("run", "build") $nodeRoot
        Invoke-Checked $npx @("node-gyp", "configure", "build") $nodeRoot
        $prebuild = Join-Path $nodeRoot "prebuilds\win32-x64"
        New-Item -ItemType Directory -Force -Path $prebuild | Out-Null
        Copy-Item -LiteralPath (Join-Path $nodeRoot "build\Release\zlink.node") -Destination (Join-Path $prebuild "zlink.node") -Force
        Copy-Item -Path (Join-Path $CorePrefix "bin\*.dll") -Destination $prebuild -Force
        New-Item -ItemType Directory -Force -Path (Join-Path $nodeRoot "provenance") | Out-Null
        Copy-Item -LiteralPath $manifest -Destination (Join-Path $nodeRoot "provenance\core-package-provenance.json") -Force
        Invoke-Checked $npm @("pack", "--pack-destination", $out) $nodeRoot
      } finally {
        $env:ZLINK_CORE_INSTALL_PREFIX = $previousPrefix
        $env:ZLINK_CORE_SOURCE = $previousSource
      }
      $package = Join-Path $out "zlink-systems-zlink-$bindingVersion.tgz"
      if (-not (Test-Path -LiteralPath $package)) { throw "Node package is missing: $package" }
    }
  }
}

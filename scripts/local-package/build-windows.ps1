param(
  [Parameter(Mandatory = $true)]
  [string]$CorePrefix,
  [string]$RepositoryRoot = "",
  [ValidateSet("cpp", "dotnet", "java", "node")]
  [string[]]$Language = @("cpp", "dotnet", "java", "node"),
  [ValidateSet("Release", "Debug")]
  [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
  $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
} else {
  $RepositoryRoot = (Resolve-Path $RepositoryRoot).Path
}
$CorePrefix = (Resolve-Path $CorePrefix).Path
$artifactRoot = Join-Path $RepositoryRoot ".artifacts\windows"
$versionFile = Join-Path $RepositoryRoot "VERSION"
$bindingsVersionFile = Join-Path $RepositoryRoot "BINDINGS_VERSION"
$coreVersion = (Select-String -LiteralPath $versionFile -Pattern "^LIBZLINK_VERSION=(.+)$").Matches.Groups[1].Value
$bindingVersion = (Select-String -LiteralPath $bindingsVersionFile -Pattern "^ZLINK_BINDINGS_VERSION=(.+)$").Matches.Groups[1].Value
$manifest = Join-Path $CorePrefix "share\zlink\core-package-provenance.json"

if ([string]::IsNullOrWhiteSpace($coreVersion) -or [string]::IsNullOrWhiteSpace($bindingVersion)) {
  throw "Unable to read Core or binding version from $RepositoryRoot"
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
  try {
    & $FileName @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$FileName failed with exit code $LASTEXITCODE" }
  } finally {
    Pop-Location
  }
}

foreach ($item in $Language) {
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
      $previousRepository = $env:MAVEN_REPOSITORY_URL
      try {
        $env:ZLINK_CORE_PACKAGE_PREFIX = $CorePrefix
        $env:ZLINK_CORE_PACKAGE_SUMMARY = $summary
        $env:MAVEN_REPOSITORY_URL = ([Uri]$maven).AbsoluteUri
        Invoke-Checked (Join-Path $RepositoryRoot "bindings\java\gradlew.bat") @(
          "--no-daemon",
          "--init-script", (Join-Path $PSScriptRoot "java\windows-package.init.gradle"),
          "clean", "publishMavenJavaPublicationToReleaseRepoRepository"
        ) (Join-Path $RepositoryRoot "bindings\java")
      } finally {
        $env:ZLINK_CORE_PACKAGE_PREFIX = $previousPrefix
        $env:ZLINK_CORE_PACKAGE_SUMMARY = $previousSummary
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

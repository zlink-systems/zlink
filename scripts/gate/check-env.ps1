[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$missing = 0

function Report-Ok([string]$Name) { Write-Output "ok $Name" }
function Report-Missing([string]$Name, [string]$Install) {
  Write-Output "missing ${Name}: $Install"
  $script:missing++
}
function Get-MajorVersion([string]$Executable) {
  try {
    $line = (& $Executable -version 2>&1 | Select-Object -First 1).ToString()
    if ($line -match '"(?<major>\d+)(?:\.|\")') { return [int]$Matches.major }
  } catch {}
  return $null
}

$java = if ($env:JAVA_HOME) { Join-Path $env:JAVA_HOME "bin\java.exe" } else { $null }
if (-not $java -or -not (Test-Path -LiteralPath $java)) {
  $javaCommand = Get-Command java.exe, java -ErrorAction SilentlyContinue | Select-Object -First 1
  $java = if ($javaCommand) { $javaCommand.Source } else { $null }
}
$javaMajor = if ($java) { Get-MajorVersion $java } else { $null }
if ($javaMajor -ge 25) { Report-Ok "JDK 25" } else { Report-Missing "JDK 25" "winget install EclipseAdoptium.Temurin.25.JDK" }

if ([string]::IsNullOrWhiteSpace($env:VCPKG_OVERLAY_PORTS)) {
  $env:VCPKG_OVERLAY_PORTS = Join-Path (Resolve-Path (Join-Path $PSScriptRoot "..\..")) "vcpkg\ports"
}
if (-not [string]::IsNullOrWhiteSpace($env:VCPKG_ROOT) -and
    (Test-Path -LiteralPath (Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake")) -and
    (Test-Path -LiteralPath $env:VCPKG_OVERLAY_PORTS)) {
  Report-Ok "VCPKG_ROOT"
} else { Report-Missing "VCPKG_ROOT" "git clone https://github.com/microsoft/vcpkg $env:LOCALAPPDATA\zlink\vcpkg; `$env:VCPKG_ROOT='$env:LOCALAPPDATA\zlink\vcpkg'" }

$browserRoot = Join-Path $env:LOCALAPPDATA "ms-playwright"
if ((Get-ChildItem -LiteralPath $browserRoot -Directory -Filter "chromium-*" -ErrorAction SilentlyContinue | Select-Object -First 1)) {
  Report-Ok "Playwright Chromium"
} else { Report-Missing "Playwright Chromium" "cd framework\languages\node; npm run browser:install" }

try { & docker info *> $null; if ($LASTEXITCODE -eq 0) { Report-Ok "docker" } else { Report-Missing "docker" "winget install Docker.DockerDesktop" } } catch { Report-Missing "docker" "winget install Docker.DockerDesktop" }
if (Get-Command dotnet.exe, dotnet -ErrorAction SilentlyContinue | Select-Object -First 1) { Report-Ok "dotnet" } else { Report-Missing "dotnet" "winget install Microsoft.DotNet.SDK.8" }

$nodeCommand = Get-Command node.exe, node -ErrorAction SilentlyContinue | Select-Object -First 1
$nodeMajor = if ($nodeCommand) { [int]((& $nodeCommand.Source -p "process.versions.node.split('.')[0]").Trim() ) } else { $null }
if ($nodeMajor -ge 20) { Report-Ok "node >= 20" } else { Report-Missing "node >= 20" "winget install OpenJS.NodeJS.LTS" }

$cmakeCommand = Get-Command cmake.exe, cmake -ErrorAction SilentlyContinue | Select-Object -First 1
$cmakeVersion = if ($cmakeCommand) { ((& $cmakeCommand.Source --version | Select-Object -First 1) -replace '^.*\s', '') } else { $null }
if ($cmakeVersion -and ([version]$cmakeVersion -ge [version]"3.20")) { Report-Ok "cmake >= 3.20" } else { Report-Missing "cmake >= 3.20" "winget install Kitware.CMake" }

if ($missing -ne 0) { exit 1 }

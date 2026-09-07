param(
  [string]$RepositoryRoot = "",
  [switch]$InstallOpenSsl,
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
$version = (Select-String -LiteralPath (Join-Path $RepositoryRoot "VERSION") -Pattern "^LIBZLINK_VERSION=(.+)$").Matches.Groups[1].Value
if ([string]::IsNullOrWhiteSpace($version)) { throw "Unable to read Core version from $RepositoryRoot" }
$artifacts = Join-Path $RepositoryRoot ".artifacts\windows"
$corePrefix = Join-Path $artifacts "install\zlink-core\$version"
$coreBuild = Join-Path $RepositoryRoot "core\build\windows-x64"
$opensslRoot = $env:OPENSSL_ROOT_DIR
if ([string]::IsNullOrWhiteSpace($opensslRoot)) {
  $opensslRoot = @("C:\Program Files\OpenSSL", "C:\Program Files\OpenSSL-Win64") |
    Where-Object { Test-Path -LiteralPath (Join-Path $_ "include\openssl\ssl.h") } |
    Select-Object -First 1
}
if ([string]::IsNullOrWhiteSpace($opensslRoot) -and $InstallOpenSsl) {
  $choco = Get-Command choco.exe, choco -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($null -eq $choco) {
    throw "-InstallOpenSsl requires Chocolatey. Install it first, or set OPENSSL_ROOT_DIR to the OpenSSL development prefix used by CI."
  }
  & $choco.Source install openssl --no-progress
  if ($LASTEXITCODE -ne 0) { throw "Chocolatey OpenSSL installation failed" }
  $opensslRoot = @("C:\Program Files\OpenSSL", "C:\Program Files\OpenSSL-Win64") |
    Where-Object { Test-Path -LiteralPath (Join-Path $_ "include\openssl\ssl.h") } |
    Select-Object -First 1
}
if ([string]::IsNullOrWhiteSpace($opensslRoot) -or -not (Test-Path -LiteralPath (Join-Path $opensslRoot "include\openssl\ssl.h"))) {
  throw "OpenSSL development files are required for the CI-equivalent Windows build. Run with -InstallOpenSsl on a Chocolatey host, or set OPENSSL_ROOT_DIR to a prefix containing include\openssl\ssl.h."
}

New-Item -ItemType Directory -Force -Path $artifacts | Out-Null
& cmake -S (Join-Path $RepositoryRoot "core") -B $coreBuild -G "Visual Studio 17 2022" -A x64 `
  "-DCMAKE_INSTALL_PREFIX=$corePrefix" "-DBUILD_SHARED=ON" "-DBUILD_STATIC=ON" `
  "-DBUILD_TESTS=OFF" "-DZLINK_CXX_STANDARD=17" "-DOPENSSL_ROOT_DIR=$opensslRoot"
if ($LASTEXITCODE -ne 0) { throw "Core configure failed" }
& cmake --build $coreBuild --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "Core build failed" }
& cmake --install $coreBuild --config $Configuration
if ($LASTEXITCODE -ne 0) { throw "Core install failed" }

$opensslBin = Join-Path $opensslRoot "bin"
if (Test-Path -LiteralPath $opensslBin) {
  Get-ChildItem -LiteralPath $opensslBin -Filter "*.dll" | Copy-Item -Destination (Join-Path $corePrefix "bin") -Force
}
$runtime = Join-Path $corePrefix "bin\zlink.dll"
if (-not (Test-Path -LiteralPath $runtime)) { throw "Core install did not produce zlink.dll: $runtime" }
$share = Join-Path $corePrefix "share\zlink"
New-Item -ItemType Directory -Force -Path $share | Out-Null
$provenancePath = Join-Path $share "core-package-provenance.json"
$files = @(Get-ChildItem -LiteralPath $corePrefix -Recurse -File |
  Where-Object { $_.FullName -ne $provenancePath } | ForEach-Object {
    [ordered]@{ path = $_.FullName.Substring($corePrefix.Length + 1).Replace("\", "/"); sha256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
}) | Sort-Object path
$provenanceJson = [ordered]@{
  schema = 1; package = "zlink-core"; version = $version; abiMajor = 0
  runtime = [ordered]@{ path = "bin/zlink.dll"; sha256 = (Get-FileHash $runtime -Algorithm SHA256).Hash.ToLowerInvariant(); soname = $null }
  source = [ordered]@{ revision = (& git -C $RepositoryRoot rev-parse HEAD).Trim(); dirty = [bool](& git -C $RepositoryRoot status --porcelain --untracked-files=no) }
  release = $null; files = $files
} | ConvertTo-Json -Depth 8
[IO.File]::WriteAllText($provenancePath, $provenanceJson,
  (New-Object Text.UTF8Encoding($false)))

& (Join-Path $PSScriptRoot "..\local-package\build-windows.ps1") -RepositoryRoot $RepositoryRoot -CorePrefix $corePrefix -Language $Language -Configuration $Configuration
if ($LASTEXITCODE -ne 0) { throw "Windows binding package build failed" }
if ($Language -contains "dotnet") {
  $env:ZLINK_LOCAL_PACKAGE_ROOT = $artifacts
  & dotnet pack (Join-Path $RepositoryRoot "framework\languages\dotnet\src\Zlink.HttpClient\Zlink.HttpClient.csproj") `
    -c $Configuration -o (Join-Path $artifacts "nuget") --nologo
  if ($LASTEXITCODE -ne 0) { throw "Framework .NET local package build failed" }
}
Write-Host "Windows local packages complete: $artifacts"

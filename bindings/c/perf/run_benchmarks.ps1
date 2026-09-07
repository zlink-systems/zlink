param(
    [string]$Pattern = "ALL",
    [string]$BuildDir = "",
    [string]$OutputFile = "",
    [int]$Runs = 1,
    [Alias("CleanBuild")]
    [switch]$Build,
    [switch]$ReuseBuild,
    [string]$ResultsDir = "",
    [string]$ResultsTag = "",
    [string]$Duration = "",
    [string]$Hwm = "",
    [string]$SendHwm = "",
    [string]$RecvHwm = "",
    [Alias("SendTimeoutMs")]
    [string]$Sndtimeo = "",
    [Alias("RecvTimeoutMs")]
    [string]$Rcvtimeo = "",
    [string]$AutoHwmProfile = "",
    [string]$IoThreads = "",
    [string]$MsgSizes = "",
    [string]$Transports = "",
    [switch]$PinCpu,
    [switch]$Help
)

$ErrorActionPreference = "Stop"

function Show-Usage {
    Write-Host @"
Usage: bindings\c\perf\run_benchmarks.ps1 [options]

Measure current zlink single-pattern performance.

Options:
  -Help                        Show this help.
  -Pattern NAME                Pattern list (comma-separated) or ALL.
  -BuildDir PATH               Official build directory (default: bindings\c\build).
  -Build, -CleanBuild          Remove the build directory and do a clean build.
  -ReuseBuild                  Reuse the existing build directory without building.
  -OutputFile PATH             Tee console logs to a file.
  -ResultsDir PATH             Override result root directory.
  -ResultsTag NAME             Optional tag in saved result filename.
  -Runs N                      Iterations per pattern/transport/size (default: 1).
  -Duration N                  Override single duration seconds (default: 5).
  -Hwm N                       Debug-only PERF_SINGLE_HWM byte override.
  -SendHwm N                   Override PERF_SINGLE_SNDHWM (fallback: -Hwm).
  -RecvHwm N                   Override PERF_SINGLE_RCVHWM (fallback: -Hwm).
  -Sndtimeo N                  Override PERF_SINGLE_SNDTIMEO_MS (default: 200).
  -Rcvtimeo N                  Override PERF_SINGLE_RCVTIMEO_MS (default: 200).
  -IoThreads N                 Set PERF_IO_THREADS.
  -MsgSizes LIST               Comma-separated message sizes.
  -Transports LIST             Comma-separated transports.
  -AutoHwmProfile NAME         Set compact, low_latency, balanced, or throughput.
  -PinCpu                      Enable PERF_TASKSET=1.

Notes:
  - result is saved under results\single\report\.
  - default build mode is incremental (configure/build without deleting the build directory).
  - -OutputFile and report output can be used together.
  - run_benchmarks.ps1 is single-only; use run_benchmarks_multi.ps1 for multi mode.
"@
}

if ($Help) {
    Show-Usage
    exit 0
}

if ($Build.IsPresent -and $ReuseBuild.IsPresent) {
    throw "-Build/-CleanBuild and -ReuseBuild are mutually exclusive."
}
$BuildMode = if ($ReuseBuild.IsPresent) { "reuse" } elseif ($Build.IsPresent) { "clean" } else { "incremental" }
$FullMatrixRequest = $Pattern.Trim().ToUpperInvariant() -eq "ALL"
if ($IoThreads -and $IoThreads -notmatch '^\d+$') {
    throw "IoThreads must be a non-negative integer."
}
if ($Duration -and $Duration -notmatch '^\d+$') {
    throw "Duration must be a positive integer."
}
if ($Duration -and [int]$Duration -lt 1) {
    throw "Duration must be >= 1."
}
if ($Hwm -and ($Hwm -notmatch '^\d+$' -or [int]$Hwm -lt 1)) {
    throw "Hwm must be a positive integer."
}
if ($SendHwm -and ($SendHwm -notmatch '^\d+$' -or [int]$SendHwm -lt 1)) {
    throw "SendHwm must be a positive integer."
}
if ($RecvHwm -and ($RecvHwm -notmatch '^\d+$' -or [int]$RecvHwm -lt 1)) {
    throw "RecvHwm must be a positive integer."
}
if ($Sndtimeo -and ($Sndtimeo -notmatch '^\d+$' -or [int]$Sndtimeo -lt 1)) {
    throw "Sndtimeo must be a positive integer."
}
if ($Rcvtimeo -and ($Rcvtimeo -notmatch '^\d+$' -or [int]$Rcvtimeo -lt 1)) {
    throw "Rcvtimeo must be a positive integer."
}
if ($MsgSizes -and $MsgSizes -notmatch '^\d+(,\d+)*$') {
    throw "MsgSizes must be a comma-separated list of integers."
}
if ($Transports -and $Transports -notmatch '^[a-z]+(,[a-z]+)*$') {
    throw "Transports must be a comma-separated list of names."
}
$CtxAutoHwmEnable = if ($env:PERF_CTX_AUTO_HWM_ENABLE) {
    $env:PERF_CTX_AUTO_HWM_ENABLE
} else {
    "1"
}
if ($CtxAutoHwmEnable -notin @("0", "1")) {
    throw "PERF_CTX_AUTO_HWM_ENABLE must be 0 or 1."
}
if (-not $AutoHwmProfile) {
    $AutoHwmProfile = $env:PERF_SINGLE_CTX_AUTO_HWM_PROFILE
}
if (-not $AutoHwmProfile) {
    $AutoHwmProfile = $env:PERF_CTX_AUTO_HWM_PROFILE
}
if (-not $AutoHwmProfile) {
    $AutoHwmProfile = "balanced"
}
if ($AutoHwmProfile -notin @("compact", "low_latency", "low-latency", "balanced", "throughput")) {
    throw "AutoHwmProfile must be compact, low_latency, balanced, or throughput."
}
if (-not $ResultsTag) {
    $ResultsTag = $env:PERF_RESULTS_TAG
}
if ($env:PERF_ALLOW_MULTI -eq "1") {
    throw "multi benchmarks are handled by bindings\\c\\perf\\run_benchmarks_multi.ps1."
}
$SinglePatterns = @(
    "PAIR",
    "PUBSUB",
    "DEALER_DEALER",
    "DEALER_ROUTER",
    "DEALER_ROUTER_REQREP",
    "ROUTER_ROUTER",
    "ROUTER_ROUTER_REQREP"
)
$SinglePatternSet = @{}
foreach ($name in $SinglePatterns) { $SinglePatternSet[$name] = $true }

if ($Runs -lt 1) {
    throw "Runs must be >= 1."
}

$ScriptDir = [System.IO.Path]::GetFullPath($PSScriptRoot)
$RootDir = $null
$ProbeDir = $ScriptDir
while ($ProbeDir) {
    if (Test-Path (Join-Path $ProbeDir ".git")) {
        $RootDir = $ProbeDir
        break
    }
    $Parent = Split-Path $ProbeDir -Parent
    if ($Parent -eq $ProbeDir) { break }
    $ProbeDir = $Parent
}
if (-not $RootDir) {
    $RootDir = Split-Path (Split-Path $ScriptDir -Parent) -Parent
}
$RootDir = [System.IO.Path]::GetFullPath($RootDir)

$OnWindows = $env:OS -eq "Windows_NT"
$CoreSource = if ($env:ZLINK_CORE_SOURCE) { $env:ZLINK_CORE_SOURCE } else { "release" }
$RepositoryVersion = (Select-String -LiteralPath (Join-Path $RootDir "VERSION") -Pattern "^LIBZLINK_VERSION=(.+)$").Matches.Groups[1].Value
if ($env:ZLINK_CORE_RELEASE_VERSION -and $env:ZLINK_CORE_RELEASE_VERSION -ne $RepositoryVersion) {
    throw "ZLINK_CORE_RELEASE_VERSION $($env:ZLINK_CORE_RELEASE_VERSION) must match repository VERSION $RepositoryVersion"
}
$CoreVersion = $RepositoryVersion
$CorePackagePrefix = if ($env:ZLINK_CORE_PACKAGE_PREFIX) {
    [System.IO.Path]::GetFullPath($env:ZLINK_CORE_PACKAGE_PREFIX)
} else {
    ""
}
if ($CoreSource -eq "release") {
    if (-not $OnWindows) {
        throw "Core release mode for the PowerShell perf runner requires Windows."
    }
    if (-not $CorePackagePrefix) {
        $FetchScript = Join-Path $RootDir "scripts\local-package\core\fetch-release.ps1"
        if (-not (Test-Path -LiteralPath $FetchScript -PathType Leaf)) {
            throw "Core release fetcher not found: $FetchScript"
        }
        $CorePackagePrefix = (& $FetchScript -Version $CoreVersion -Platform "windows-x64" |
            Select-Object -Last 1).ToString().Trim()
    }
    if (-not $CorePackagePrefix -or
        -not (Test-Path -LiteralPath (Join-Path $CorePackagePrefix "share\zlink\core-package-provenance.json") -PathType Leaf)) {
        throw "Core release prefix is missing provenance: $CorePackagePrefix"
    }
    $CoreManifest = Get-Content -LiteralPath (Join-Path $CorePackagePrefix "share\zlink\core-package-provenance.json") -Raw |
        ConvertFrom-Json
    if ($CoreManifest.version -ne $CoreVersion) {
        throw "Core release prefix version $($CoreManifest.version) does not match $CoreVersion"
    }
} elseif ($CoreSource -ne "local") {
    throw "ZLINK_CORE_SOURCE must be release or local: $CoreSource"
}
$CoreDir = if ($CoreSource -eq "release") { $CorePackagePrefix } else { Join-Path $RootDir "core" }

$CMakeSourceDir = Join-Path $RootDir "bindings\c"
$OfficialBuildDir = if ($OnWindows) {
    Join-Path $CMakeSourceDir "build\windows-x64"
} else {
    Join-Path $CMakeSourceDir "build"
}
if (-not $BuildDir) {
    $BuildDir = $OfficialBuildDir
}
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
if (-not $BuildDir.Equals($OfficialBuildDir, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Build directory must be exactly: $OfficialBuildDir"
}

$BenchComparisonScript = Join-Path $ScriptDir "single\run_comparison.py"
$BenchComparisonScript = [System.IO.Path]::GetFullPath($BenchComparisonScript)
if (-not (Test-Path $BenchComparisonScript)) {
    throw "comparison script not found: $BenchComparisonScript"
}

if (-not $Pattern) {
    throw "Pattern name is required."
}

$PatternList = @()
if ($FullMatrixRequest) {
    $PatternList = @($SinglePatterns)
    $Pattern = ($PatternList -join ",")
} else {
    $PatternList = $Pattern.Split(",") | ForEach-Object { $_.Trim().ToUpperInvariant() } | Where-Object { $_ -ne "" }
    if ($PatternList.Count -eq 0) {
        throw "Error: no valid pattern specified."
    }
    foreach ($p in $PatternList) {
        if (-not $SinglePatternSet.ContainsKey($p)) {
            throw "Unsupported single pattern: $p"
        }
    }
    $Pattern = ($PatternList -join ",")
}

if (-not $ResultsDir) {
    $ResultsDir = $env:PERF_RESULTS_DIR
}
if (-not $ResultsDir) {
    $ResultsDir = Join-Path $ScriptDir "results"
}
if ($ResultsDir) {
    $ResultsDir = [System.IO.Path]::GetFullPath($ResultsDir)
}

$ResultFile = ""
$Timestamp = (Get-Date).ToString("yyyyMMdd_HHmmss")
$Name = "perf_c_single_windows_${Timestamp}"
if ($ResultsTag) {
    $Name = "${Name}_${ResultsTag}"
}
$ResultSuite = "single"
$ResultFile = Join-Path (Join-Path (Join-Path $ResultsDir $ResultSuite) "report") "${Name}.txt"
if ($OutputFile) {
    $OutputFile = [System.IO.Path]::GetFullPath($OutputFile)
}

if ($ResultFile -and $OutputFile -and ($ResultFile -ieq $OutputFile)) {
    throw "-OutputFile cannot point to the same file as result output."
}

function Cleanup-OldResultDirs {
    param(
        [string]$RootPath
    )

    if (-not $RootPath -or -not (Test-Path $RootPath)) {
        return
    }

    $RetentionRaw = $env:PERF_RESULTS_RETENTION_DAYS
    if (-not $RetentionRaw) {
        $RetentionRaw = "90"
    }

    $Retention = 0
    if (-not [int]::TryParse($RetentionRaw, [ref]$Retention)) {
        return
    }
    if ($Retention -le 0) {
        return
    }

    $Cutoff = (Get-Date).ToUniversalTime().AddDays(-$Retention)

    Get-ChildItem -Path $RootPath -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d{8}$' } |
        ForEach-Object {
            $dirDate = $null
            if ([datetime]::TryParseExact($_.Name, "yyyyMMdd", $null,
                                          [System.Globalization.DateTimeStyles]::AssumeUniversal,
                                          [ref]$dirDate)) {
                if ($dirDate -lt $Cutoff) {
                    Remove-Item -Recurse -Force $_.FullName
                }
            }
        }
}

if ($ResultsDir) {
    Cleanup-OldResultDirs -RootPath $ResultsDir
}

function Resolve-ConfiguredCoreBuildDir {
    param([string]$BuildRoot)

    if ($CoreSource -eq "release") {
        return $CorePackagePrefix
    }

    $Configured = ""
    $CacheFile = Join-Path $BuildRoot "CMakeCache.txt"
    if (Test-Path $CacheFile) {
        $Line = Get-Content -LiteralPath $CacheFile |
            Select-String -Pattern '^ZLINK_C_CORE_BUILD_DIR:[^=]*=' |
            Select-Object -Last 1
        if ($Line) {
            $Configured = $Line.Line.Substring($Line.Line.IndexOf('=') + 1).Trim()
        }
    }
    if (-not $Configured) {
        $Configured = if ($OnWindows) {
            Join-Path $RootDir "core\build\windows-x64"
        } else {
            Join-Path $RootDir "core\build"
        }
    }
    return [System.IO.Path]::GetFullPath($Configured)
}

function Resolve-CoreRuntime {
    param([string]$CoreRoot)

    $Candidates = @(
        (Join-Path $CoreRoot "bin\Release\zlink.dll"),
        (Join-Path $CoreRoot "lib\Release\zlink.dll"),
        (Join-Path $CoreRoot "bin\zlink.dll"),
        (Join-Path $CoreRoot "lib\zlink.dll")
    )
    foreach ($Candidate in $Candidates) {
        if (Test-Path $Candidate) {
            return [System.IO.Path]::GetFullPath($Candidate)
        }
    }
    return $null
}

function Resolve-OpenSslRoot {
    param([string]$CoreRoot)

    $Candidates = @()
    if ($env:OPENSSL_ROOT_DIR) {
        $Candidates += $env:OPENSSL_ROOT_DIR
    }

    $CacheFile = Join-Path $CoreRoot "CMakeCache.txt"
    if (Test-Path $CacheFile) {
        $Line = Get-Content -LiteralPath $CacheFile |
            Select-String -Pattern '^OPENSSL_ROOT_DIR:[^=]*=' |
            Select-Object -First 1
        if ($Line) {
            $Candidates += $Line.Line.Substring($Line.Line.IndexOf('=') + 1).Trim()
        }
    }

    if ($env:VCPKG_ROOT) {
        $Triplet = if ($env:VCPKG_DEFAULT_TRIPLET) { $env:VCPKG_DEFAULT_TRIPLET } else { "x64-windows-static" }
        $Candidates += Join-Path $env:VCPKG_ROOT "installed\$Triplet"
    }

    foreach ($Candidate in $Candidates) {
        if (-not $Candidate) { continue }
        if (Test-Path (Join-Path $Candidate "include\openssl\opensslv.h")) {
            return [System.IO.Path]::GetFullPath($Candidate)
        }
    }
    return $null
}

function Invoke-CoreRuntimeBuild {
    param([string]$CoreRoot)

    $CoreCache = Join-Path $CoreRoot "CMakeCache.txt"
    $CMakeGenerator = if ($env:CMAKE_GENERATOR) { $env:CMAKE_GENERATOR } else { "Visual Studio 17 2022" }
    $CMakeArch = if ($env:CMAKE_ARCH) { $env:CMAKE_ARCH } else { "x64" }
    if (-not (Test-Path $CoreCache)) {
        New-Item -ItemType Directory -Force -Path $CoreRoot | Out-Null
        $ConfigureArgs = @(
            "-S", (Join-Path $RootDir "core"),
            "-B", $CoreRoot,
            "-G", $CMakeGenerator,
            "-DCMAKE_BUILD_TYPE=Release",
            "-DBUILD_TESTS=OFF",
            "-DWITH_DOCS=OFF",
            "-DWITH_TLS=ON",
            "-DBUILD_BENCHMARKS=ON",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
        )
        if ($CMakeGenerator -like "Visual Studio*") {
            $ConfigureArgs += @("-A", $CMakeArch)
        }
        if ($OpenSslRoot) {
            $ConfigureArgs += @("-DOPENSSL_ROOT_DIR=$OpenSslRoot", "-DCMAKE_PREFIX_PATH=$OpenSslRoot")
        }
        Write-Host "Configuring core runtime: $CoreRoot"
        & cmake @ConfigureArgs
        if ($LASTEXITCODE -ne 0) {
            throw "Core CMake configuration failed."
        }
    }

    Write-Host "Building core runtime: $CoreRoot"
    & cmake --build $CoreRoot --config Release
    if ($LASTEXITCODE -ne 0) {
        throw "Core runtime build failed."
    }
}

function Prepare-CoreRuntime {
    param([string]$CoreRoot)

    $Runtime = Resolve-CoreRuntime -CoreRoot $CoreRoot
    if ($CoreSource -eq "release") {
        if (-not $Runtime) {
            throw "Core release runtime zlink.dll was not found: $CoreRoot"
        }
        Write-Host "Perf Core release prefix: $CoreRoot"
        Write-Host "Perf runtime zlink.dll: $Runtime"
        return $Runtime
    }

    $NeedsBuild = -not $Runtime
    if ($Runtime) {
        $RuntimeItem = Get-Item -LiteralPath $Runtime
        $NewerSource = Get-ChildItem -Path (Join-Path $RootDir "core\src"), (Join-Path $RootDir "core\include") -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.LastWriteTimeUtc -gt $RuntimeItem.LastWriteTimeUtc } |
            Select-Object -First 1
        if ($NewerSource) {
            $NeedsBuild = $true
            Write-Host "Core runtime is older than source: $($NewerSource.FullName)"
        }
    }
    if ($NeedsBuild) {
        Invoke-CoreRuntimeBuild -CoreRoot $CoreRoot
        $Runtime = Resolve-CoreRuntime -CoreRoot $CoreRoot
        if (-not $Runtime) {
            throw "Core runtime zlink.dll was not found after build: $CoreRoot"
        }
    }
    Write-Host "Perf core build dir: $CoreRoot"
    Write-Host "Perf runtime zlink.dll: $Runtime"
    return $Runtime
}

function Resolve-SingleBuildTargets {
    param([string[]]$Patterns)

    $TargetMap = @{
        "PAIR" = @("perf_pair")
        "PUBSUB" = @("perf_pubsub")
        "DEALER_DEALER" = @("perf_dealer_dealer")
        "DEALER_ROUTER" = @("perf_dealer_router")
        "DEALER_ROUTER_REQREP" = @("perf_dealer_router_reqrep")
        "ROUTER_ROUTER" = @("perf_router_router")
        "ROUTER_ROUTER_REQREP" = @("perf_router_router_reqrep")
    }
    $Targets = New-Object 'System.Collections.Generic.List[string]'
    foreach ($PatternName in $Patterns) {
        foreach ($TargetName in $TargetMap[$PatternName]) {
            if (-not $Targets.Contains($TargetName)) {
                $Targets.Add($TargetName)
            }
        }
    }
    return @($Targets.ToArray())
}

$CoreBuildDir = Resolve-ConfiguredCoreBuildDir -BuildRoot $BuildDir
$OpenSslRoot = Resolve-OpenSslRoot -CoreRoot $CoreBuildDir
if ($OnWindows -and -not $OpenSslRoot) {
    throw "OpenSSL was not found. Set OPENSSL_ROOT_DIR or VCPKG_ROOT."
}
if ($OpenSslRoot) {
    Write-Host "Perf OpenSSL root: $OpenSslRoot"
}
Prepare-CoreRuntime -CoreRoot $CoreBuildDir | Out-Null

if ($BuildMode -eq "clean" -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning build directory: $BuildDir"
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}

if ($BuildMode -ne "reuse") {
    $CacheFile = Join-Path $BuildDir "CMakeCache.txt"
    if (Test-Path $CacheFile) {
        $CacheSource = Get-Content -LiteralPath $CacheFile |
            Select-String -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=' |
            Select-Object -First 1
        if ($CacheSource) {
            $ConfiguredSource = [System.IO.Path]::GetFullPath(
                $CacheSource.Line.Substring($CacheSource.Line.IndexOf('=') + 1).Trim()
            )
            if (-not $ConfiguredSource.Equals($CMakeSourceDir, [System.StringComparison]::OrdinalIgnoreCase)) {
                Write-Host "Build cache source mismatch detected; resetting: $BuildDir"
                Remove-Item -LiteralPath $BuildDir -Recurse -Force
            }
        }
    }

    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    $CMakeGenerator = if ($env:CMAKE_GENERATOR) { $env:CMAKE_GENERATOR } else { "Visual Studio 17 2022" }
    $CMakeArch = if ($env:CMAKE_ARCH) { $env:CMAKE_ARCH } else { "x64" }
    $CMakeArgs = @(
        "-S", $CMakeSourceDir,
        "-B", $BuildDir,
        "-G", $CMakeGenerator,
        "-DCMAKE_BUILD_TYPE=Release",
        "-DENABLE_LTO=OFF",
        "-DZLINK_CORE_DIR=$CoreDir",
        "-DZLINK_C_CORE_BUILD_DIR=$CoreBuildDir",
        "-DZLINK_C_BUILD_BENCHMARKS=ON",
        "-DZLINK_C_BUILD_SAMPLES=OFF"
    )
    if ($OpenSslRoot) {
        $CMakeArgs += @("-DOPENSSL_ROOT_DIR=$OpenSslRoot", "-DCMAKE_PREFIX_PATH=$OpenSslRoot")
    }
    if ($CMakeGenerator -like "Visual Studio*") {
        $CMakeArgs += @("-A", $CMakeArch)
    }

    Write-Host "Using CMake source directory: $CMakeSourceDir"
    Write-Host "Using core build directory: $CoreBuildDir"
    & cmake @CMakeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configuration failed."
    }

    $BuildTargets = @(Resolve-SingleBuildTargets -Patterns $PatternList)
    if ($BuildTargets.Count -eq 0) {
        throw "No benchmark build targets resolved for the selected patterns."
    }
    Write-Host ("Building benchmark targets: " + ($BuildTargets -join ", "))
    & cmake --build $BuildDir --config Release --target $BuildTargets
    if ($LASTEXITCODE -ne 0) {
        throw "Benchmark build failed."
    }
} else {
    Write-Host "Reusing build directory: $BuildDir"
}

function Resolve-PythonExecutable {
    if ($env:PYTHON -and (Test-Path $env:PYTHON)) {
        return $env:PYTHON
    }

    foreach ($name in @("python", "python3")) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if (-not $cmd -or -not $cmd.Source -or -not (Test-Path $cmd.Source)) {
            continue
        }
        if ($cmd.Source -like "*\WindowsApps\*") {
            continue
        }
        try {
            & $cmd.Source --version *> $null
            if ($LASTEXITCODE -eq 0) {
                return $cmd.Source
            }
        } catch {
            continue
        }
    }

    $pyLauncher = Get-Command py -ErrorAction SilentlyContinue
    if ($pyLauncher -and $pyLauncher.Source -and (Test-Path $pyLauncher.Source)) {
        try {
            $resolved = & $pyLauncher.Source -3 -c "import sys; print(sys.executable)" 2>$null
            $resolved = ($resolved | Select-Object -First 1).Trim()
            if ($resolved -and (Test-Path $resolved)) {
                return $resolved
            }
        } catch {
        }
    }

    return $null
}

$PythonExe = Resolve-PythonExecutable
if (-not $PythonExe) {
    throw "Python not found. Install Python 3 or ensure it is on PATH."
}

Write-Host "Using Python: $PythonExe"

$RunArgs = @($Pattern, "--build-dir", $BuildDir, "--runs", $Runs.ToString())
if ($PinCpu) {
    $RunArgs += "--pin-cpu"
}

if ($ResultsDir) {
    $RunArgs += @("--results-dir", $ResultsDir)
}
if ($ResultsTag) {
    $RunArgs += @("--results-tag", $ResultsTag)
}
$RunArgs += @("--result-file", $ResultFile)

$RunEnv = @{}
if (-not $IoThreads) { $IoThreads = $env:PERF_IO_THREADS }
if (-not $MsgSizes) { $MsgSizes = $env:PERF_MSG_SIZES }
if (-not $Transports) { $Transports = $env:PERF_TRANSPORTS }
if (-not $Duration) { $Duration = $env:PERF_SINGLE_DURATION_SECONDS }
if (-not $Hwm) { $Hwm = $env:PERF_SINGLE_HWM }
if (-not $SendHwm) { $SendHwm = $env:PERF_SINGLE_SNDHWM }
if (-not $RecvHwm) { $RecvHwm = $env:PERF_SINGLE_RCVHWM }
if (-not $Sndtimeo) { $Sndtimeo = $env:PERF_SINGLE_SNDTIMEO_MS }
if (-not $Rcvtimeo) { $Rcvtimeo = $env:PERF_SINGLE_RCVTIMEO_MS }
if (-not $Duration) { $Duration = "5" }
if (-not $Sndtimeo) { $Sndtimeo = "200" }
if (-not $Rcvtimeo) { $Rcvtimeo = "200" }

if ($Sndtimeo -notmatch '^\d+$' -or [int]$Sndtimeo -lt 1) {
    throw "Sndtimeo must be a positive integer."
}
if ($Rcvtimeo -notmatch '^\d+$' -or [int]$Rcvtimeo -lt 1) {
    throw "Rcvtimeo must be a positive integer."
}
$AllowManualSocketOverrides = if ($env:PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES) {
    $env:PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES
} elseif ($env:PERF_ALLOW_MANUAL_SOCKET_OVERRIDES) {
    $env:PERF_ALLOW_MANUAL_SOCKET_OVERRIDES
} else {
    "0"
}
if (($Hwm -or $SendHwm -or $RecvHwm) -and $AllowManualSocketOverrides -ne "1") {
    throw "manual HWM overrides are debug-only. Set PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES=1 first."
}

if ($IoThreads) {
    $RunEnv["PERF_IO_THREADS"] = $IoThreads
}
if ($MsgSizes) {
    $RunEnv["PERF_MSG_SIZES"] = $MsgSizes
}
if ($Transports) {
    $RunEnv["PERF_TRANSPORTS"] = $Transports
}
if ($Duration) {
    if ($Duration -notmatch '^\d+$' -or [int]$Duration -lt 1) {
        throw "Duration must be a positive integer."
    }
    $RunArgs += @("--duration", $Duration)
    $RunEnv["PERF_SINGLE_DURATION_SECONDS"] = $Duration
}
if ($Hwm) {
    $RunEnv["PERF_SINGLE_HWM"] = $Hwm
}
if ($SendHwm) {
    $RunEnv["PERF_SINGLE_SNDHWM"] = $SendHwm
}
if ($RecvHwm) {
    $RunEnv["PERF_SINGLE_RCVHWM"] = $RecvHwm
}
$RunEnv["PERF_SINGLE_SNDTIMEO_MS"] = $Sndtimeo
$RunEnv["PERF_SINGLE_RCVTIMEO_MS"] = $Rcvtimeo
$RunEnv["PERF_CTX_AUTO_HWM_ENABLE"] = $CtxAutoHwmEnable
$RunEnv["PERF_CTX_AUTO_HWM_PROFILE"] = $AutoHwmProfile
$RunEnv["PYTHONUNBUFFERED"] = "1"
if ($AllowManualSocketOverrides -eq "1") {
    $RunEnv["PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES"] = "1"
}
if ($PinCpu) {
    $RunEnv["PERF_TASKSET"] = "1"
}
if ($BuildMode -eq "reuse") {
    $RunEnv["PERF_NO_AUTOBUILD"] = "1"
}
if ($FullMatrixRequest) {
    $RunEnv["PERF_FULL_MATRIX"] = "1"
}

function Get-ValueOrDefault {
    param(
        [string]$Value,
        [string]$DefaultValue
    )
    if ([string]::IsNullOrWhiteSpace($Value)) {
        return $DefaultValue
    }
    return $Value
}

function Show-EffectiveOption {
    param(
        [string]$Key,
        [string]$Value
    )
    Write-Host ("- {0}: {1}" -f $Key, $Value)
}

$EffectiveSendHwm = if ($SendHwm) { $SendHwm } elseif ($Hwm) { $Hwm } else { "" }
$EffectiveRecvHwm = if ($RecvHwm) { $RecvHwm } elseif ($Hwm) { $Hwm } else { "" }
$EffectiveIoThreads = if ($IoThreads) { $IoThreads } else { "default(binary=1)" }

Write-Host ""
Write-Host "## Effective Options (runner)"
Show-EffectiveOption "pattern" $Pattern
Show-EffectiveOption "build_dir" $BuildDir
Show-EffectiveOption "build_mode" $BuildMode
Show-EffectiveOption "reuse_build" $(if ($BuildMode -eq "reuse") { "1" } else { "0" })
Show-EffectiveOption "clean_build" $(if ($BuildMode -eq "clean") { "1" } else { "0" })
Show-EffectiveOption "runs" $Runs.ToString()
Show-EffectiveOption "duration_seconds" $Duration
Show-EffectiveOption "hwm" (Get-ValueOrDefault -Value $Hwm -DefaultValue "default(binary)")
Show-EffectiveOption "send_hwm" (Get-ValueOrDefault -Value $EffectiveSendHwm -DefaultValue "default(binary)")
Show-EffectiveOption "recv_hwm" (Get-ValueOrDefault -Value $EffectiveRecvHwm -DefaultValue "default(binary)")
Show-EffectiveOption "sndtimeo_ms" $Sndtimeo
Show-EffectiveOption "rcvtimeo_ms" $Rcvtimeo
Show-EffectiveOption "pin_cpu" $(if ($PinCpu) { "1" } else { "0" })
Show-EffectiveOption "io_threads" $EffectiveIoThreads
Show-EffectiveOption "msg_sizes" (Get-ValueOrDefault -Value $MsgSizes -DefaultValue "default(benchmark)")
Show-EffectiveOption "transports" (Get-ValueOrDefault -Value $Transports -DefaultValue "default(benchmark)")
Show-EffectiveOption "results_dir" $ResultsDir
Show-EffectiveOption "results_tag" (Get-ValueOrDefault -Value $ResultsTag -DefaultValue "none")
Show-EffectiveOption "result_file" $ResultFile
Show-EffectiveOption "output_file" (Get-ValueOrDefault -Value $OutputFile -DefaultValue "none")
Show-EffectiveOption "comparison_script" $BenchComparisonScript
Show-EffectiveOption "python" $PythonExe
Write-Host ""
Write-Host "## Effective Env (runner)"
foreach ($key in ($RunEnv.Keys | Sort-Object)) {
    Show-EffectiveOption $key $RunEnv[$key]
}
Write-Host ""
Write-Host "Running benchmarks..."
Write-Host ""

$PreviousEnv = @{}
foreach ($key in $RunEnv.Keys) {
    $PreviousEnv[$key] = [Environment]::GetEnvironmentVariable($key, "Process")
    [Environment]::SetEnvironmentVariable($key, $RunEnv[$key], "Process")
}

try {
    if ($OutputFile) {
        $OutputDir = Split-Path $OutputFile -Parent
        if ($OutputDir -and -not (Test-Path $OutputDir)) {
            New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
        }
        & $PythonExe $BenchComparisonScript @RunArgs | Tee-Object -FilePath $OutputFile
    } else {
        & $PythonExe $BenchComparisonScript @RunArgs
    }
    $ExitCode = $LASTEXITCODE
} finally {
    foreach ($key in $RunEnv.Keys) {
        [Environment]::SetEnvironmentVariable($key, $PreviousEnv[$key], "Process")
    }
}

exit $ExitCode

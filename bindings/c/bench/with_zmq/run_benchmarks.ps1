param(
    [string]$Pattern = "ALL",
    [string]$BuildDir = "",
    [string]$OutputFile = "",
    [string]$ResultsDir = "",
    [string]$ResultsTag = "",
    [int]$Runs = 1,
    [Alias("duration")]
    [string]$Duration = "",
    [string]$Hwm = "",
    [string]$SendHwm = "",
    [string]$RecvHwm = "",
    [Alias("sndtimeo", "sndtimeoMs")]
    [string]$Sndtimeo = "",
    [Alias("rcvtimeo", "rcvtimeoMs")]
    [string]$Rcvtimeo = "",
    [string]$IoThreads = "",
    [string]$MsgSizes = "",
    [string]$Transports = "",
    [switch]$PinCpu,
    [switch]$ReuseBuild,
    [switch]$CleanBuild,
    [switch]$Help
)

$ErrorActionPreference = "Stop"

function Show-Usage {
    Write-Host @"
Usage: core\bench\with_zmq\run_benchmarks.ps1 [options]

Run SINGLE benchmark comparisons (libzmq vs zlink).

Options:
  -Help                        Show this help.
  -Pattern NAME                Pattern list (comma-separated) or ALL.
  -BuildDir PATH               Build directory (default: core\build\windows-x64).
  -ReuseBuild                  Reuse existing build directory as-is (skip configure/build).
  -CleanBuild                  Remove build directory and do a clean build.
  -OutputFile PATH             Tee console logs to a file.
  -ResultsDir PATH             Override result root directory.
  -ResultsTag NAME             Optional tag in saved result filename.
  -Runs N                      Iterations per pattern/transport/size (default: 1).
  -Duration N                  Override single duration seconds (default: 5).
  -Hwm N                       Override PERF_SINGLE_HWM (default: 1000 in binary).
  -SendHwm N                   Override PERF_SINGLE_SNDHWM (fallback: -Hwm).
  -RecvHwm N                   Override PERF_SINGLE_RCVHWM (fallback: -Hwm).
  -Sndtimeo N                  Override PERF_SINGLE_SNDTIMEO_MS (default: 200).
  -Rcvtimeo N                  Override PERF_SINGLE_RCVTIMEO_MS (default: 200).
  -IoThreads N                 Set PERF_IO_THREADS.
  -MsgSizes LIST               Comma-separated message sizes.
  -Transports LIST             Comma-separated transports.
  -PinCpu                      Enable taskset mode on Linux runner.
Notes:
  - Supported patterns: PAIR,PUBSUB,DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER
  - Removed patterns: STREAM,GATEWAY,SPOT
  - Supported transports: tcp,inproc (ipc is not supported on Windows).
  - Removed transports: ws,wss,tls
  - result is saved under results\single\report\.
"@
}

if ($Help) {
    Show-Usage
    exit 0
}

if ($ReuseBuild -and $CleanBuild) {
    throw "ReuseBuild and CleanBuild are mutually exclusive."
}

function Resolve-RootDir {
    param([string]$StartDir)

    $probe = $StartDir
    while ($probe) {
        if (Test-Path (Join-Path $probe ".git")) {
            return $probe
        }
        $parent = Split-Path $probe -Parent
        if ($parent -eq $probe) { break }
        $probe = $parent
    }

    return [System.IO.Path]::GetFullPath((Join-Path $StartDir "../../.."))
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
        }
        catch {
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
        }
        catch {
        }
    }

    return $null
}

function Cleanup-OldResultDirs {
    param([string]$RootPath)

    if (-not $RootPath -or -not (Test-Path $RootPath)) {
        return
    }

    $retentionRaw = $env:BENCH_RESULTS_RETENTION_DAYS
    if (-not $retentionRaw) {
        $retentionRaw = $env:PERF_RESULTS_RETENTION_DAYS
    }
    if (-not $retentionRaw) {
        $retentionRaw = "90"
    }

    $retention = 0
    if (-not [int]::TryParse($retentionRaw, [ref]$retention)) {
        return
    }
    if ($retention -le 0) {
        return
    }

    $cutoff = (Get-Date).ToUniversalTime().AddDays(-$retention)
    Get-ChildItem -Path $RootPath -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d{8}$' } |
        ForEach-Object {
            $dirDate = $null
            if ([datetime]::TryParseExact($_.Name, "yyyyMMdd", $null,
                                          [System.Globalization.DateTimeStyles]::AssumeUniversal,
                                          [ref]$dirDate)) {
                if ($dirDate -lt $cutoff) {
                    Remove-Item -Recurse -Force $_.FullName
                }
            }
        }
}

function Validate-PositiveIntString {
    param(
        [string]$Value,
        [string]$Name
    )

    if (-not $Value) {
        return
    }
    if ($Value -notmatch '^\d+$') {
        throw "$Name must be a positive integer."
    }
    if ([int]$Value -lt 1) {
        throw "$Name must be >= 1."
    }
}

$ScriptDir = $PSScriptRoot
$RootDir = Resolve-RootDir -StartDir $ScriptDir
$RootDir = [System.IO.Path]::GetFullPath($RootDir)
$ComparisonScript = [System.IO.Path]::GetFullPath((Join-Path $ScriptDir "single\run_comparison.py"))
if (-not (Test-Path $ComparisonScript)) {
    throw "comparison script not found: $ComparisonScript"
}

if (-not $BuildDir) {
    $BuildDir = Join-Path $RootDir "core\build\windows-x64"
}
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
if (-not $BuildDir.StartsWith($RootDir, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Build directory must be inside repo root: $RootDir"
}

if (-not $ResultsDir) {
    $ResultsDir = Join-Path $ScriptDir "results"
}
$ResultsDir = [System.IO.Path]::GetFullPath($ResultsDir)

if (-not $Pattern) {
    throw "Pattern name is required."
}

$allowedPatterns = @(
    "PAIR",
    "PUBSUB",
    "DEALER_DEALER",
    "DEALER_ROUTER",
    "ROUTER_ROUTER",
)

$singleBuildTargets = @(
    "comp_std_zmq_pair",
    "comp_zlink_pair",
    "comp_std_zmq_pubsub",
    "comp_zlink_pubsub",
    "comp_std_zmq_dealer_dealer",
    "comp_zlink_dealer_dealer",
    "comp_std_zmq_dealer_router",
    "comp_zlink_dealer_router",
    "comp_std_zmq_router_router",
    "comp_zlink_router_router",
)

$patternList = @()
if ($Pattern.Trim().ToUpperInvariant() -eq "ALL") {
    $patternList = $allowedPatterns
}
else {
    $patternList = $Pattern.Split(",") |
        ForEach-Object { $_.Trim().ToUpperInvariant() } |
        Where-Object { $_ -ne "" }
    if ($patternList.Count -eq 0) {
        throw "Error: no valid pattern specified."
    }

    foreach ($p in $patternList) {
        switch ($p) {
            { $_ -in $allowedPatterns } { }
            "STREAM" { throw "STREAM is removed from with_zmq/single." }
            "GATEWAY" { throw "GATEWAY is removed from with_zmq/single." }
            "SPOT" { throw "SPOT is removed from with_zmq/single." }
            { $_ -like "MULTI_*" } {
                throw "run_benchmarks.ps1 is single-pattern mode only. Use run_benchmarks_multi.ps1 for MULTI_* patterns."
            }
            default {
                throw "Unsupported pattern '$p'. Supported: $($allowedPatterns -join ',')"
            }
        }
    }
}
$PatternCsv = ($patternList -join ",")

if ($Runs -lt 1) {
    throw "Runs must be >= 1."
}

Validate-PositiveIntString -Value $Duration -Name "Duration"
Validate-PositiveIntString -Value $Hwm -Name "Hwm"
Validate-PositiveIntString -Value $SendHwm -Name "SendHwm"
Validate-PositiveIntString -Value $RecvHwm -Name "RecvHwm"
Validate-PositiveIntString -Value $Sndtimeo -Name "Sndtimeo"
Validate-PositiveIntString -Value $Rcvtimeo -Name "Rcvtimeo"

if ($IoThreads -and $IoThreads -notmatch '^\d+$') {
    throw "IoThreads must be a non-negative integer."
}
if ($MsgSizes -and $MsgSizes -notmatch '^\d+(,\d+)*$') {
    throw "MsgSizes must be a comma-separated list of integers."
}
if ($Transports -and $Transports -notmatch '^[a-z]+(,[a-z]+)*$') {
    throw "Transports must be a comma-separated list of names."
}

if (-not $IoThreads) { $IoThreads = if ($env:PERF_IO_THREADS) { $env:PERF_IO_THREADS } else { $env:BENCH_IO_THREADS } }
if (-not $MsgSizes) { $MsgSizes = if ($env:PERF_MSG_SIZES) { $env:PERF_MSG_SIZES } else { $env:BENCH_MSG_SIZES } }
if (-not $Transports) { $Transports = if ($env:PERF_TRANSPORTS) { $env:PERF_TRANSPORTS } else { $env:BENCH_TRANSPORTS } }
if (-not $Duration) { $Duration = if ($env:PERF_SINGLE_DURATION_SECONDS) { $env:PERF_SINGLE_DURATION_SECONDS } else { "5" } }
if (-not $Hwm) { $Hwm = $env:PERF_SINGLE_HWM }
if (-not $SendHwm) { $SendHwm = $env:PERF_SINGLE_SNDHWM }
if (-not $RecvHwm) { $RecvHwm = $env:PERF_SINGLE_RCVHWM }
if (-not $Sndtimeo) { $Sndtimeo = $env:PERF_SINGLE_SNDTIMEO_MS }
if (-not $Rcvtimeo) { $Rcvtimeo = if ($env:PERF_SINGLE_RCVTIMEO_MS) { $env:PERF_SINGLE_RCVTIMEO_MS } else { $env:PERF_SINGLE_PUBSUB_RCVTIMEO_MS } }

if ($Transports) {
    $transportList = $Transports.Split(",") | ForEach-Object { $_.Trim().ToLowerInvariant() } | Where-Object { $_ -ne "" }
    if ($transportList.Count -eq 0) {
        throw "Transports must contain at least one value."
    }
    foreach ($tr in $transportList) {
        switch ($tr) {
            "tcp" { }
            "inproc" { }
            "ipc" { throw "unsupported transport 'ipc' on Windows. Supported: tcp,inproc" }
            "ws" { throw "transport 'ws' is removed from with_zmq/single." }
            "wss" { throw "transport 'wss' is removed from with_zmq/single." }
            "tls" { throw "transport 'tls' is removed from with_zmq/single." }
            default { throw "unsupported transport '$tr'. Supported: tcp,inproc" }
        }
    }
    $Transports = ($transportList -join ",")
}
else {
    $Transports = "tcp,inproc"
}

$timestamp = (Get-Date).ToString("yyyyMMdd_HHmmss")
$name = "perf_windows_${timestamp}"
if ($ResultsTag) {
    $name = "${name}_${ResultsTag}"
}
$resultFile = Join-Path (Join-Path (Join-Path $ResultsDir "single") "report") "${name}.txt"

if ($OutputFile) {
    $OutputFile = [System.IO.Path]::GetFullPath($OutputFile)
}
if ($OutputFile -and ($OutputFile -ieq $resultFile)) {
    throw "-OutputFile cannot point to the same file as result output."
}

if ($ReuseBuild) {
    if (-not (Test-Path $BuildDir)) {
        throw "ReuseBuild requires an existing build directory: $BuildDir"
    }
    Write-Host "Reusing build directory: $BuildDir"
}
else {
    if ($CleanBuild -and (Test-Path $BuildDir)) {
        Write-Host "Cleaning build directory: $BuildDir"
        Remove-Item -Recurse -Force $BuildDir
    }

    $cmakeGenerator = if ($env:CMAKE_GENERATOR) { $env:CMAKE_GENERATOR } else { "Visual Studio 17 2022" }
    $cmakeArch = if ($env:CMAKE_ARCH) { $env:CMAKE_ARCH } else { "x64" }

    Write-Host "Configuring CMake..."
    $cmakeArgs = @(
        "-S", "$RootDir",
        "-B", "$BuildDir",
        "-G", "$cmakeGenerator",
        "-A", "$cmakeArch",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_BENCHMARKS=ON",
        "-DZLINK_BUILD_TESTS=OFF",
        "-DZLINK_BUILD_BENCH_ZMQ=ON",
        "-DZLINK_BUILD_BENCH_ZLINK=ON",
        "-DZLINK_BUILD_BENCH_BEAST=OFF",
        "-DZLINK_BUILD_BENCH_STREAMCOMPARE=OFF",
        "-DZLINK_BUILD_BENCH_ROUTER_COMPARE=OFF",
        "-DZLINK_CXX_STANDARD=17"
    )
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configuration failed"
    }

    Write-Host "Building..."
    & cmake --build $BuildDir --config Release --target @singleBuildTargets
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed"
    }
}

Cleanup-OldResultDirs -RootPath $ResultsDir

$pythonExe = Resolve-PythonExecutable
if (-not $pythonExe) {
    throw "Python not found. Install Python 3 or ensure it is on PATH."
}

$runArgs = @(
    $PatternCsv,
    "--build-dir", $BuildDir,
    "--runs", $Runs.ToString(),
    "--transports", $Transports,
    "--results-dir", $ResultsDir,
    "--results-tag", $ResultsTag,
    "--result-file", $resultFile
)
if ($PinCpu) {
    $runArgs += "--pin-cpu"
}
if ($MsgSizes) {
    $runArgs += @("--msg-sizes", $MsgSizes)
}

$runEnv = @{}
$runEnv["PYTHONUNBUFFERED"] = "1"
if ($IoThreads) {
    $runEnv["PERF_IO_THREADS"] = $IoThreads
    $runEnv["BENCH_IO_THREADS"] = $IoThreads
}
if ($MsgSizes) {
    $runEnv["PERF_MSG_SIZES"] = $MsgSizes
    $runEnv["BENCH_MSG_SIZES"] = $MsgSizes
}
if ($Transports) {
    $runEnv["PERF_TRANSPORTS"] = $Transports
    $runEnv["BENCH_TRANSPORTS"] = $Transports
}
if ($Duration) {
    $runEnv["PERF_SINGLE_DURATION_SECONDS"] = $Duration
}
if ($Hwm) {
    $runEnv["PERF_SINGLE_HWM"] = $Hwm
}
if ($SendHwm) {
    $runEnv["PERF_SINGLE_SNDHWM"] = $SendHwm
}
if ($RecvHwm) {
    $runEnv["PERF_SINGLE_RCVHWM"] = $RecvHwm
}
if ($Sndtimeo) {
    $runEnv["PERF_SINGLE_SNDTIMEO_MS"] = $Sndtimeo
}
if ($Rcvtimeo) {
    $runEnv["PERF_SINGLE_RCVTIMEO_MS"] = $Rcvtimeo
}
if ($ReuseBuild) {
    $runEnv["BENCH_NO_AUTOBUILD"] = "1"
    $runEnv["PERF_NO_AUTOBUILD"] = "1"
}
if ($env:PERF_DISABLE_RESOURCE_METRICS) {
    $runEnv["PERF_DISABLE_RESOURCE_METRICS"] = $env:PERF_DISABLE_RESOURCE_METRICS
}

foreach ($key in $runEnv.Keys) {
    Set-Item -Path "env:$key" -Value $runEnv[$key]
}

$startTime = Get-Date
$exitCode = 1
try {
    if ($OutputFile) {
        $OutputDir = Split-Path $OutputFile -Parent
        if ($OutputDir -and -not (Test-Path $OutputDir)) {
            New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
        }
        & $pythonExe $ComparisonScript @runArgs | Tee-Object -FilePath $OutputFile
    }
    else {
        & $pythonExe $ComparisonScript @runArgs
    }
    $exitCode = $LASTEXITCODE
}
finally {
    foreach ($key in $runEnv.Keys) {
        Remove-Item -Path "env:$key" -ErrorAction SilentlyContinue
    }
    $elapsed = [int]((Get-Date) - $startTime).TotalSeconds
    if ($env:BENCH_SUPPRESS_TOTAL_TIME -ne "1" -and $env:PERF_SUPPRESS_TOTAL_TIME -ne "1") {
        Write-Host "Total benchmark time: ${elapsed}s (exit=$exitCode)"
    }
}

exit $exitCode

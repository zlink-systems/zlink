param(
    [string]$Pattern = "ALL",
    [string]$BuildDir = "",
    [string]$OutputFile = "",
    [int]$Runs = 1,
    [switch]$Build,
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
  -BuildDir PATH               Build directory (default: core\build\windows-x64).
  -Build                       Force clean build (default is reuse-build).
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
  -PinCpu                      Enable PERF_TASKSET=1.

Notes:
  - result is saved under results\single\report\.
  - reuse-build is always enabled unless -Build is provided.
  - -OutputFile and report output can be used together.
  - run_benchmarks.ps1 is single-only; use run_benchmarks_multi.ps1 for multi mode.
"@
}

if ($Help) {
    Show-Usage
    exit 0
}

$UseReuseBuild = -not $Build.IsPresent
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
if (-not $ResultsTag) {
    $ResultsTag = $env:PERF_RESULTS_TAG
}
if ($env:PERF_ALLOW_MULTI -eq "1") {
    throw "multi benchmarks are handled by bindings\\c\\perf\\run_benchmarks_multi.ps1."
}
$SinglePatterns = @("PAIR", "PUBSUB", "DEALER_DEALER", "DEALER_ROUTER", "ROUTER_ROUTER")
$SinglePatternSet = @{}
foreach ($name in $SinglePatterns) { $SinglePatternSet[$name] = $true }

if ($Runs -lt 1) {
    throw "Runs must be >= 1."
}

$ScriptDir = $PSScriptRoot
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

if (-not $BuildDir) {
    $BuildDir = Join-Path $RootDir "core\build\windows-x64"
}
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
if (-not $BuildDir.StartsWith($RootDir, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Build directory must be inside repo root: $RootDir"
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
if ($Pattern.Trim().ToUpperInvariant() -eq "ALL") {
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

$NeedResultsDir = $true
if ($NeedResultsDir -and -not $ResultsDir) {
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

function Resolve-BenchmarkBinDir {
    param([string]$BuildRoot)

    $Candidates = @(
        (Join-Path $BuildRoot "bin\Release"),
        (Join-Path $BuildRoot "bin\Debug"),
        (Join-Path $BuildRoot "bin"),
        $BuildRoot
    )

    foreach ($Candidate in $Candidates) {
        if (Test-Path (Join-Path $Candidate "perf_pair.exe")) {
            return $Candidate
        }
    }
    return $Candidates[0]
}

$NeedConfigureBuild = -not $UseReuseBuild
if ($UseReuseBuild) {
    $BenchBinDir = Resolve-BenchmarkBinDir -BuildRoot $BuildDir
    $HasSingle = Test-Path (Join-Path $BenchBinDir "perf_pair.exe")
    $HasMulti = Test-Path (Join-Path $BenchBinDir "comp_src_dealer_dealer_client.exe")
    if ((Test-Path $BuildDir) -and ($HasSingle -or $HasMulti)) {
        Write-Host "Reusing build directory: $BuildDir"
    } else {
        Write-Host "Reusable build not found. Configuring/building: $BuildDir"
        $NeedConfigureBuild = $true
        $UseReuseBuild = $false
    }
}

if ($NeedConfigureBuild) {
    Write-Host "Cleaning build directory: $BuildDir"
    if (Test-Path $BuildDir) {
        Remove-Item -Recurse -Force $BuildDir
    }

    $CMakeGenerator = if ($env:CMAKE_GENERATOR) { $env:CMAKE_GENERATOR } else { "Visual Studio 17 2022" }
    $CMakeArch = if ($env:CMAKE_ARCH) { $env:CMAKE_ARCH } else { "x64" }

    Write-Host "Configuring CMake..."

    $CMakeArgs = @(
        "-S", "$RootDir",
        "-B", "$BuildDir",
        "-G", "$CMakeGenerator",
        "-A", "$CMakeArch",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_BENCHMARKS=ON",
        "-DZLINK_BUILD_TESTS=OFF",
        "-DZLINK_BUILD_BENCH_ZMQ=OFF",
        "-DZLINK_BUILD_BENCH_ZLINK=ON",
        "-DZLINK_BUILD_BENCH_BEAST=OFF",
        "-DZLINK_BUILD_BENCH_STREAMCOMPARE=OFF",
        "-DZLINK_BUILD_BENCH_ROUTER_COMPARE=OFF",
        "-DZLINK_CXX_STANDARD=17"
    )

    & cmake @CMakeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configuration failed"
    }

    Write-Host "Building..."
    & cmake --build $BuildDir --config Release
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed"
    }
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
if ($PinCpu) {
    $RunEnv["PERF_TASKSET"] = "1"
}
if ($UseReuseBuild) {
    $RunEnv["PERF_NO_AUTOBUILD"] = "1"
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

$BuildMode = if ($UseReuseBuild) { "reuse" } else { "clean" }
$EffectiveSendHwm = if ($SendHwm) { $SendHwm } elseif ($Hwm) { $Hwm } else { "" }
$EffectiveRecvHwm = if ($RecvHwm) { $RecvHwm } elseif ($Hwm) { $Hwm } else { "" }
$EffectiveIoThreads = if ($IoThreads) { $IoThreads } else { "default(binary=1)" }

Write-Host ""
Write-Host "## Effective Options (runner)"
Show-EffectiveOption "pattern" $Pattern
Show-EffectiveOption "build_dir" $BuildDir
Show-EffectiveOption "build_mode" $BuildMode
Show-EffectiveOption "reuse_build" $(if ($UseReuseBuild) { "1" } else { "0" })
Show-EffectiveOption "clean_build" $(if ($UseReuseBuild) { "0" } else { "1" })
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

foreach ($key in $RunEnv.Keys) {
    Set-Item -Path "env:$key" -Value $RunEnv[$key]
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
        Remove-Item -Path "env:$key" -ErrorAction SilentlyContinue
    }
}

exit $ExitCode

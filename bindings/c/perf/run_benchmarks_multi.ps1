param(
    [string]$Pattern = "ALL",
    [string]$BuildDir = "",
    [string]$OutputFile = "",
    [int]$Runs = 1,
    [switch]$Build,
    [string]$ResultsDir = "",
    [string]$ResultsTag = "",
    [switch]$Callback,
    [string]$IoThreads = "",
    [string]$MsgSizes = "",
    [string]$Transports = "",
    [switch]$PinCpu,
    [Alias("MultiDurationSeconds")]
    [int]$Duration = 5,
    [Alias("MultiClients")]
    [string]$Clients = "",
    [Alias("MultiHwm")]
    [string]$Hwm = "",
    [Alias("MultiSndHwm")]
    [string]$SendHwm = "",
    [Alias("MultiRcvHwm")]
    [string]$RecvHwm = "",
    [Alias("MultiSndtimeoMs")]
    [string]$SendTimeoutMs = "",
    [Alias("MultiRcvtimeoMs")]
    [string]$RecvTimeoutMs = "",
    [Alias("MultiConnectConcurrency")]
    [string]$ConnectConcurrency = "",
    [Alias("MultiTransportTransitionMs")]
    [int]$TransportTransitionMs = 3000,
    [Alias("MultiPatternTransitionMs")]
    [int]$PatternTransitionMs = 3000,
    [Alias("MultiServerReadyTimeoutMs")]
    [int]$ServerReadyTimeoutMs = 10000,
    [Alias("MultiConnectReadyTimeoutMs")]
    [int]$ConnectReadyTimeoutMs = 1000,
    [Alias("MultiMonitorHwm")]
    [int]$MonitorHwm = 4096000,
    [Alias("MultiServerShutdownTimeoutMs")]
    [int]$ServerShutdownTimeoutMs = 5000,
    [Alias("MultiServerBindPort")]
    [int]$ServerBindPort = 0,
    [switch]$Help
)

$ErrorActionPreference = "Stop"

function Show-Usage {
    Write-Host @"
Usage: bindings\c\perf\run_benchmarks_multi.ps1 [options]

Run only multi-socket benchmark patterns.
This script invokes the shared comparison runner directly.

Options:
  -Pattern NAME                Pattern list (comma-separated) or ALL.
                               Alias: stream/streams => STREAM
  -BuildDir PATH               Official build directory (default: platform-specific bindings\c\build\windows-x64 on Windows).
  -OutputFile PATH             Tee console logs to file.
  -Runs N                      Iterations per configuration (default: 1).
  -Build                       Clean and rebuild benchmark targets (default is incremental).
  -ResultsDir PATH             Override result root directory.
  -ResultsTag NAME             Optional tag appended to result filename.
  -IoThreads N                 Set PERF_IO_THREADS.
                               Default multi io-threads are non-stream=4, stream=4.
  -MsgSizes LIST               Comma-separated sizes.
  -Transports LIST             Comma-separated transports.
  -PinCpu                      Enable PERF_TASKSET=1.
  -Duration N                  Override PERF_MULTI_DURATION_SECONDS.
  -Clients N                   Override PERF_MULTI_CLIENTS (default: 100, stream=10000).
  -Hwm N                       Debug-only override PERF_MULTI_HWM.
                               Requires PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES=1.
  -SendHwm N                   Debug-only override PERF_MULTI_SNDHWM (fallback: -Hwm).
  -RecvHwm N                   Debug-only override PERF_MULTI_RCVHWM (fallback: -Hwm).
  -SendTimeoutMs N             Override PERF_MULTI_SNDTIMEO_MS.
  -RecvTimeoutMs N             Override PERF_MULTI_RCVTIMEO_MS.
  -ConnectConcurrency N        Override PERF_MULTI_CONNECT_CONCURRENCY.
  -TransportTransitionMs N     Transport transition cooldown(ms).
  -PatternTransitionMs N       Pattern transition cooldown(ms).
  -ServerReadyTimeoutMs N      Server READY wait timeout(ms).
  -ConnectReadyTimeoutMs N     Client/server connect-ready wait timeout(ms).
  -MonitorHwm N                Monitor socket HWM (default: 4096000).
  -ServerShutdownTimeoutMs N   Server shutdown wait timeout(ms).
  -ServerBindPort N            Server bind port (0=auto).
"@
}

if ($Help) {
    Show-Usage
    exit 0
}

$DurationExplicit = $PSBoundParameters.ContainsKey("Duration")
$TransportTransitionExplicit = $PSBoundParameters.ContainsKey("TransportTransitionMs")
$PatternTransitionExplicit = $PSBoundParameters.ContainsKey("PatternTransitionMs")
$ServerReadyExplicit = $PSBoundParameters.ContainsKey("ServerReadyTimeoutMs")
$ConnectReadyExplicit = $PSBoundParameters.ContainsKey("ConnectReadyTimeoutMs")
$MonitorHwmExplicit = $PSBoundParameters.ContainsKey("MonitorHwm")
$ServerShutdownExplicit = $PSBoundParameters.ContainsKey("ServerShutdownTimeoutMs")
$ServerBindPortExplicit = $PSBoundParameters.ContainsKey("ServerBindPort")

function Get-EnvironmentDefault {
    param(
        [string]$Primary,
        [string]$Fallback,
        [string]$Default
    )
    $Value = [Environment]::GetEnvironmentVariable($Primary, "Process")
    if (-not $Value -and $Fallback) {
        $Value = [Environment]::GetEnvironmentVariable($Fallback, "Process")
    }
    if (-not $Value) {
        $Value = $Default
    }
    return $Value
}

if (-not $DurationExplicit) {
    $Duration = [int](Get-EnvironmentDefault -Primary "PERF_MULTI_DURATION_SECONDS" -Fallback "PERF_DURATION_SECONDS" -Default "5")
}
if (-not $TransportTransitionExplicit) {
    $TransportTransitionMs = [int](Get-EnvironmentDefault -Primary "PERF_MULTI_TRANSPORT_TRANSITION_MS" -Fallback "PERF_TRANSPORT_TRANSITION_MS" -Default "3000")
}
if (-not $PatternTransitionExplicit) {
    $PatternTransitionMs = [int](Get-EnvironmentDefault -Primary "PERF_MULTI_PATTERN_TRANSITION_MS" -Fallback "PERF_PATTERN_TRANSITION_MS" -Default "3000")
}
if (-not $ServerReadyExplicit) {
    $ServerReadyTimeoutMs = [int](Get-EnvironmentDefault -Primary "PERF_MULTI_SERVER_READY_TIMEOUT_MS" -Fallback "PERF_SERVER_READY_TIMEOUT_MS" -Default "10000")
}
if (-not $ConnectReadyExplicit) {
    $ConnectReadyTimeoutMs = [int](Get-EnvironmentDefault -Primary "PERF_MULTI_CONNECT_READY_TIMEOUT_MS" -Fallback "PERF_CONNECT_READY_TIMEOUT_MS" -Default "10000")
}
if (-not $MonitorHwmExplicit) {
    $MonitorHwm = [int](Get-EnvironmentDefault -Primary "PERF_MULTI_MONITOR_HWM" -Fallback "PERF_MONITOR_HWM" -Default "4096000")
}
if (-not $ServerShutdownExplicit) {
    $ServerShutdownTimeoutMs = [int](Get-EnvironmentDefault -Primary "PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS" -Fallback "PERF_SERVER_SHUTDOWN_TIMEOUT_MS" -Default "5000")
}
if (-not $ServerBindPortExplicit) {
    $ServerBindPort = [int](Get-EnvironmentDefault -Primary "PERF_MULTI_SERVER_BIND_PORT" -Fallback "PERF_SERVER_BIND_PORT" -Default "0")
}
if (-not $SendTimeoutMs) {
    $SendTimeoutMs = Get-EnvironmentDefault -Primary "PERF_MULTI_SNDTIMEO_MS" -Fallback "PERF_SNDTIMEO_MS" -Default "200"
}
if (-not $RecvTimeoutMs) {
    $RecvTimeoutMs = Get-EnvironmentDefault -Primary "PERF_MULTI_RCVTIMEO_MS" -Fallback "PERF_RCVTIMEO_MS" -Default "200"
}

if ($Duration -lt 1) { throw "Duration must be >= 1." }
if ($TransportTransitionMs -lt 0) { throw "TransportTransitionMs must be >= 0." }
if ($PatternTransitionMs -lt 0) { throw "PatternTransitionMs must be >= 0." }
if ($ServerReadyTimeoutMs -lt 0) { throw "ServerReadyTimeoutMs must be >= 0." }
if ($ConnectReadyTimeoutMs -lt 0) { throw "ConnectReadyTimeoutMs must be >= 0." }
if ($MonitorHwm -lt 0) { throw "MonitorHwm must be >= 0." }
if ($ServerShutdownTimeoutMs -lt 0) { throw "ServerShutdownTimeoutMs must be >= 0." }
if ($ServerBindPort -lt 0 -or $ServerBindPort -gt 65535) {
    throw "ServerBindPort must be in range 0..65535."
}
if ($IoThreads -and $IoThreads -notmatch '^\d+$') { throw "IoThreads must be a non-negative integer." }
if ($Callback.IsPresent) { throw "-Callback is no longer supported." }
if ($Hwm -and ($Hwm -notmatch '^\d+$' -or [int]$Hwm -lt 1)) { throw "Hwm must be a positive integer." }
if ($SendHwm -and ($SendHwm -notmatch '^\d+$' -or [int]$SendHwm -lt 1)) { throw "SendHwm must be a positive integer." }
if ($RecvHwm -and ($RecvHwm -notmatch '^\d+$' -or [int]$RecvHwm -lt 1)) { throw "RecvHwm must be a positive integer." }
if ($MsgSizes -and $MsgSizes -notmatch '^\d+(,\d+)*$') { throw "MsgSizes must be a comma-separated list of integers." }
if ($Transports -and $Transports -notmatch '^[a-z]+(,[a-z]+)*$') { throw "Transports must be a comma-separated list of names." }
if ($SendTimeoutMs -notmatch '^\d+$' -or [int]$SendTimeoutMs -lt 1) { throw "SendTimeoutMs must be a positive integer." }
if ($RecvTimeoutMs -notmatch '^\d+$' -or [int]$RecvTimeoutMs -lt 1) { throw "RecvTimeoutMs must be a positive integer." }
$DefaultPatterns = @(
    "DEALER_DEALER",
    "DEALER_ROUTER_SENDSEND",
    "ROUTER_ROUTER_SENDSEND",
    "DEALER_ROUTER_REQREP",
    "ROUTER_ROUTER_REQREP",
    "ROUTER_ROUTER_ONEWAY",
    "PUBSUB",
    "STREAM"
)

function Add-UniquePattern {
    param(
        [System.Collections.Generic.List[string]]$List,
        [string]$PatternName
    )
    if ([string]::IsNullOrWhiteSpace($PatternName)) { return }
    if (-not $List.Contains($PatternName)) {
        $List.Add($PatternName)
    }
}

function Expand-AndAddPatternAlias {
    param(
        [System.Collections.Generic.List[string]]$List,
        [string]$RawPattern
    )
    $p = $RawPattern.Trim().ToUpperInvariant()
    if (-not $p) { return }
    if ($p.StartsWith("MULTI_")) {
        $p = $p.Substring(6)
    }
    switch ($p) {
        "DEALER_ROUTER" {
            Add-UniquePattern -List $List -PatternName "DEALER_ROUTER_SENDSEND"
            break
        }
        "ROUTER_ROUTER" {
            Add-UniquePattern -List $List -PatternName "ROUTER_ROUTER_SENDSEND"
            break
        }
        "STREAM" {
            Add-UniquePattern -List $List -PatternName "STREAM"
            break
        }
        "STREAMS" {
            Add-UniquePattern -List $List -PatternName "STREAM"
            break
        }
        default {
            Add-UniquePattern -List $List -PatternName $p
            break
        }
    }
}

$ExpandedPatterns = New-Object 'System.Collections.Generic.List[string]'
if ($Pattern.Trim().ToUpperInvariant() -eq "ALL") {
    foreach ($p in $DefaultPatterns) {
        Expand-AndAddPatternAlias -List $ExpandedPatterns -RawPattern $p
    }
} else {
    foreach ($part in $Pattern.Split(",")) {
        Expand-AndAddPatternAlias -List $ExpandedPatterns -RawPattern $part
    }
}

$PatternList = @()
foreach ($p in $ExpandedPatterns) {
    $PatternList += $p
}
if ($PatternList.Count -eq 0) {
    throw "No valid pattern specified."
}
$PatternCsv = ($PatternList -join ",")

$AllowManualSocketOverrides = if ($env:PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES) {
    $env:PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES
} elseif ($env:PERF_ALLOW_MANUAL_SOCKET_OVERRIDES) {
    $env:PERF_ALLOW_MANUAL_SOCKET_OVERRIDES
} else {
    "0"
}
if (($Hwm -or $SendHwm -or $RecvHwm) -and $AllowManualSocketOverrides -ne "1") {
    throw "manual HWM overrides are debug-only. Set PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES=1 first."
}

if ($Runs -lt 1) { throw "Runs must be >= 1." }

$ScriptDir = [System.IO.Path]::GetFullPath($PSScriptRoot)
$RootDir = [System.IO.Path]::GetFullPath((Join-Path $ScriptDir "..\..\.."))
$CMakeSourceDir = Join-Path $RootDir "bindings\c"
$OnWindows = $env:OS -eq "Windows_NT"
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

$CoreBuildCandidates = @()
if ($env:ZLINK_C_CORE_BUILD_DIR) {
    $CoreBuildCandidates += [System.IO.Path]::GetFullPath($env:ZLINK_C_CORE_BUILD_DIR)
}
if ($OnWindows) {
    $CoreBuildCandidates += Join-Path $RootDir "core\build\windows-x64"
    $CoreBuildCandidates += Join-Path $RootDir "core\build"
} else {
    $CoreBuildCandidates += Join-Path $RootDir "core\build"
}
$CoreBuildDir = $null
foreach ($Candidate in $CoreBuildCandidates) {
    if ((Test-Path (Join-Path $Candidate "bin\Release\zlink.dll")) -or
        (Test-Path (Join-Path $Candidate "lib\Release\zlink.dll")) -or
        (Test-Path (Join-Path $Candidate "bin\zlink.dll")) -or
        (Test-Path (Join-Path $Candidate "lib\zlink.dll"))) {
        $CoreBuildDir = $Candidate
        break
    }
}
if (-not $CoreBuildDir) {
    $CoreBuildDir = if ($OnWindows -and (Test-Path (Join-Path $RootDir "core\build\windows-x64\CMakeCache.txt"))) {
        Join-Path $RootDir "core\build\windows-x64"
    } else {
        Join-Path $RootDir "core\build"
    }
}

function Resolve-OpenSslRoot {
    param([string]$CoreRoot)

    $Candidates = @()
    if ($env:OPENSSL_ROOT_DIR) {
        $Candidates += $env:OPENSSL_ROOT_DIR
    }
    $CoreCacheFile = Join-Path $CoreRoot "CMakeCache.txt"
    if (Test-Path $CoreCacheFile) {
        $OpenSslLine = Get-Content -LiteralPath $CoreCacheFile |
            Select-String -Pattern '^OPENSSL_ROOT_DIR:[^=]*=' |
            Select-Object -First 1
        if ($OpenSslLine) {
            $Candidates += $OpenSslLine.Line.Substring($OpenSslLine.Line.IndexOf('=') + 1)
        }
    }
    if ($env:VCPKG_ROOT) {
        $Triplet = if ($env:VCPKG_DEFAULT_TRIPLET) { $env:VCPKG_DEFAULT_TRIPLET } else { "x64-windows-static" }
        $Candidates += Join-Path $env:VCPKG_ROOT "installed\$Triplet"
    }
    if ($env:USERPROFILE) {
        $Candidates += Join-Path $env:USERPROFILE "vcpkg\installed\x64-windows-static"
    }

    foreach ($Candidate in $Candidates) {
        if (-not $Candidate) { continue }
        $Candidate = [System.IO.Path]::GetFullPath($Candidate)
        if ((Test-Path (Join-Path $Candidate "include\openssl\ssl.h")) -and
            (Test-Path (Join-Path $Candidate "lib\libssl.lib"))) {
            return $Candidate
        }
    }
    return $null
}

$OpenSslRoot = Resolve-OpenSslRoot -CoreRoot $CoreBuildDir
if (-not $OpenSslRoot) {
    throw "OpenSSL was not found. Set OPENSSL_ROOT_DIR to a Windows OpenSSL installation."
}

$CMakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $CMakeCommand -or -not $CMakeCommand.Source) {
    throw "CMake not found. Install CMake or ensure it is on PATH."
}
$CMakeExe = $CMakeCommand.Source

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

function Prepare-CoreRuntime {
    param([string]$CoreRoot)

    $Runtime = Resolve-CoreRuntime -CoreRoot $CoreRoot
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
        $CacheFile = Join-Path $CoreRoot "CMakeCache.txt"
        $Generator = if ($env:CMAKE_GENERATOR) { $env:CMAKE_GENERATOR } else { "Visual Studio 17 2022" }
        $Architecture = if ($env:CMAKE_ARCH) { $env:CMAKE_ARCH } else { "x64" }
        if (-not (Test-Path $CacheFile)) {
            New-Item -ItemType Directory -Force -Path $CoreRoot | Out-Null
            $ConfigureArgs = @(
                "-S", (Join-Path $RootDir "core"),
                "-B", $CoreRoot,
                "-G", $Generator,
                "-DCMAKE_BUILD_TYPE=Release",
                "-DBUILD_TESTS=OFF",
                "-DWITH_DOCS=OFF",
                "-DWITH_TLS=ON",
                "-DBUILD_BENCHMARKS=ON",
                "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
            )
            if ($Generator -like "Visual Studio*") {
                $ConfigureArgs += @("-A", $Architecture)
            }
            if ($OpenSslRoot) {
                $ConfigureArgs += @("-DOPENSSL_ROOT_DIR=$OpenSslRoot", "-DCMAKE_PREFIX_PATH=$OpenSslRoot")
            }
            Write-Host "Configuring core runtime: $CoreRoot"
            & $CMakeExe @ConfigureArgs
            if ($LASTEXITCODE -ne 0) {
                throw "Core CMake configuration failed."
            }
        }
        Write-Host "Building core runtime: $CoreRoot"
        & $CMakeExe --build $CoreRoot --config Release
        if ($LASTEXITCODE -ne 0) {
            throw "Core runtime build failed."
        }
        $Runtime = Resolve-CoreRuntime -CoreRoot $CoreRoot
        if (-not $Runtime) {
            throw "Core runtime zlink.dll was not found after build: $CoreRoot"
        }
    }
    Write-Host "Perf core build dir: $CoreRoot"
    Write-Host "Perf runtime zlink.dll: $Runtime"
}

function Resolve-MultiBuildTargets {
    param([string[]]$Patterns)

    $TargetMap = @{
        "DEALER_DEALER" = @("comp_src_dealer_dealer_server", "comp_src_dealer_dealer_client")
        "DEALER_ROUTER_SENDSEND" = @("comp_src_dealer_router_sendsend_server", "comp_src_dealer_router_sendsend_client")
        "ROUTER_ROUTER_SENDSEND" = @("comp_src_router_router_sendsend_server", "comp_src_router_router_sendsend_client")
        "DEALER_ROUTER_REQREP" = @("comp_src_dealer_router_reqrep_server", "comp_src_dealer_router_reqrep_client")
        "ROUTER_ROUTER_REQREP" = @("comp_src_router_router_reqrep_server", "comp_src_router_router_reqrep_client")
        "ROUTER_ROUTER_ONEWAY" = @("comp_src_router_router_oneway_server", "comp_src_router_router_oneway_client")
        "PUBSUB" = @("comp_src_pubsub_server", "comp_src_pubsub_client")
        "STREAM" = @("comp_src_stream_server", "perf_stream_client")
    }
    $Targets = New-Object 'System.Collections.Generic.List[string]'
    foreach ($PatternName in $Patterns) {
        if (-not $TargetMap.ContainsKey($PatternName)) {
            continue
        }
        foreach ($TargetName in $TargetMap[$PatternName]) {
            if (-not $Targets.Contains($TargetName)) {
                $Targets.Add($TargetName)
            }
        }
    }
    return @($Targets.ToArray())
}

function Invoke-MultiBindingBuild {
    param(
        [string]$BuildRoot,
        [string]$CoreRoot,
        [string[]]$Patterns
    )

    $CacheFile = Join-Path $BuildRoot "CMakeCache.txt"
    if ($Build.IsPresent -and (Test-Path $BuildRoot)) {
        Write-Host "Cleaning build directory: $BuildRoot"
        Remove-Item -LiteralPath $BuildRoot -Recurse -Force
    }

    if (Test-Path $CacheFile) {
        $CacheSource = Get-Content -LiteralPath $CacheFile |
            Select-String -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=' |
            Select-Object -First 1
        if ($CacheSource) {
            $ConfiguredSource = $CacheSource.Line.Substring($CacheSource.Line.IndexOf('=') + 1)
            $ConfiguredSource = [System.IO.Path]::GetFullPath($ConfiguredSource)
            if (-not $ConfiguredSource.Equals($CMakeSourceDir, [System.StringComparison]::OrdinalIgnoreCase)) {
                Write-Host "Build cache source mismatch detected; resetting: $BuildRoot"
                Remove-Item -LiteralPath $BuildRoot -Recurse -Force
            }
        }
    }

    New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
    $Generator = if ($env:CMAKE_GENERATOR) { $env:CMAKE_GENERATOR } else { "Visual Studio 17 2022" }
    $Architecture = if ($env:CMAKE_ARCH) { $env:CMAKE_ARCH } else { "x64" }
    $ConfigureArgs = @(
        "-S", $CMakeSourceDir,
        "-B", $BuildRoot,
        "-G", $Generator,
        "-DCMAKE_BUILD_TYPE=Release",
        "-DENABLE_LTO=OFF",
        "-DZLINK_CORE_DIR=$RootDir\core",
        "-DZLINK_C_CORE_BUILD_DIR=$CoreRoot",
        "-DOPENSSL_ROOT_DIR=$OpenSslRoot",
        "-DCMAKE_PREFIX_PATH=$OpenSslRoot",
        "-DZLINK_C_BUILD_BENCHMARKS=ON",
        "-DZLINK_C_BUILD_SAMPLES=OFF"
    )
    if ($Generator -like "Visual Studio*") {
        $ConfigureArgs += @("-A", $Architecture)
    }

    Write-Host "Using CMake source directory: $CMakeSourceDir"
    Write-Host "Using core build directory: $CoreRoot"
    & $CMakeExe @ConfigureArgs
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configuration failed."
    }

    $BuildTargets = @(Resolve-MultiBuildTargets -Patterns $Patterns)
    if ($BuildTargets.Count -eq 0) {
        throw "No benchmark build targets resolved for the selected patterns."
    }
    $BuildArgs = @("--build", $BuildRoot, "--config", "Release", "--target") + $BuildTargets
    Write-Host ("Building benchmark targets: " + ($BuildTargets -join ", "))
    & $CMakeExe @BuildArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Benchmark build failed."
    }
}

Prepare-CoreRuntime -CoreRoot $CoreBuildDir
Invoke-MultiBindingBuild -BuildRoot $BuildDir -CoreRoot $CoreBuildDir -Patterns $PatternList
$DefaultMsgSizes = "64,256,1024,4096,65536,131072"
$DefaultStreamMsgSizes = "64,256,1024,65536"
$EffectiveResultsDir = if ($ResultsDir) {
    $ResultsDir
} elseif ($env:PERF_RESULTS_DIR) {
    $env:PERF_RESULTS_DIR
} else {
    Join-Path $ScriptDir "results"
}
$ResultsTag = if ($ResultsTag) { $ResultsTag } else { $env:PERF_RESULTS_TAG }
$EffectiveMsgSizes = if ($MsgSizes) {
    $MsgSizes
} elseif ($env:PERF_MSG_SIZES) {
    $env:PERF_MSG_SIZES
} else {
    $DefaultMsgSizes
}
$EffectiveTransports = if ($Transports) {
    $Transports
} else {
    "tcp,tls,ws,wss"
}
$EffectiveStreamMsgSizes = if ($env:PERF_MULTI_STREAM_MSG_SIZES) {
    $env:PERF_MULTI_STREAM_MSG_SIZES
} elseif ($env:PERF_STREAM_MSG_SIZES) {
    $env:PERF_STREAM_MSG_SIZES
} else {
    $DefaultStreamMsgSizes
}
$BenchComparisonScript = Join-Path $ScriptDir "run_comparison.py"
if (-not (Test-Path $BenchComparisonScript)) {
    throw "comparison script not found: $BenchComparisonScript"
}
$RunCooldownMs = [int](Get-EnvironmentDefault -Primary "PERF_MULTI_RUN_COOLDOWN_MS" -Fallback "PERF_RUN_COOLDOWN_MS" -Default "3000")
$CtxAutoHwmEnable = if ($env:PERF_CTX_AUTO_HWM_ENABLE) { $env:PERF_CTX_AUTO_HWM_ENABLE } else { "1" }
$CtxAutoHwmProfile = if ($env:PERF_MULTI_CTX_AUTO_HWM_PROFILE) {
    $env:PERF_MULTI_CTX_AUTO_HWM_PROFILE
} elseif ($env:PERF_CTX_AUTO_HWM_PROFILE) {
    $env:PERF_CTX_AUTO_HWM_PROFILE
} else {
    "balanced"
}
if ($RunCooldownMs -lt 0) { throw "RunCooldownMs must be >= 0." }
if ($CtxAutoHwmEnable -notin @("0", "1")) { throw "PERF_CTX_AUTO_HWM_ENABLE must be 0 or 1." }
if ($CtxAutoHwmProfile -notin @("compact", "low_latency", "low-latency", "balanced", "throughput")) {
    throw "CtxAutoHwmProfile must be compact, low_latency, low-latency, balanced, or throughput."
}
$DisableResourceMetrics = if ($env:PERF_DISABLE_RESOURCE_METRICS) { $env:PERF_DISABLE_RESOURCE_METRICS } else { "0" }

$RunEnv = @{}
$RunEnv["PERF_ALLOW_MULTI"] = "1"
$RunEnv["PERF_POLICY"] = "1"
$RunEnv["PERF_RESULTS_DIR"] = $EffectiveResultsDir
$RunEnv["PERF_TRANSPORTS"] = $EffectiveTransports
$RunEnv["PERF_MSG_SIZES"] = $EffectiveMsgSizes
$RunEnv["PERF_MULTI_STREAM_MSG_SIZES"] = $EffectiveStreamMsgSizes
$RunEnv["PERF_MULTI_DURATION_SECONDS"] = $Duration.ToString()
$RunEnv["PERF_MULTI_SNDTIMEO_MS"] = $SendTimeoutMs
$RunEnv["PERF_MULTI_RCVTIMEO_MS"] = $RecvTimeoutMs
$RunEnv["PERF_MULTI_TRANSPORT_TRANSITION_MS"] = $TransportTransitionMs.ToString()
$RunEnv["PERF_MULTI_PATTERN_TRANSITION_MS"] = $PatternTransitionMs.ToString()
$RunEnv["PERF_MULTI_SERVER_READY_TIMEOUT_MS"] = $ServerReadyTimeoutMs.ToString()
$RunEnv["PERF_MULTI_CONNECT_READY_TIMEOUT_MS"] = $ConnectReadyTimeoutMs.ToString()
$RunEnv["PERF_MULTI_MONITOR_HWM"] = $MonitorHwm.ToString()
$RunEnv["PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS"] = $ServerShutdownTimeoutMs.ToString()
$RunEnv["PERF_MULTI_SERVER_BIND_PORT"] = $ServerBindPort.ToString()
$RunEnv["PERF_MULTI_RUN_COOLDOWN_MS"] = $RunCooldownMs.ToString()
$RunEnv["PERF_CTX_AUTO_HWM_ENABLE"] = $CtxAutoHwmEnable
$RunEnv["PERF_CTX_AUTO_HWM_PROFILE"] = $CtxAutoHwmProfile
$RunEnv["PERF_DISABLE_RESOURCE_METRICS"] = $DisableResourceMetrics
$RunEnv["PYTHONUNBUFFERED"] = "1"
if ($Pattern.Trim().ToUpperInvariant() -eq "ALL") {
    $RunEnv["PERF_FULL_MATRIX"] = "1"
}
if ($Clients) { $RunEnv["PERF_MULTI_CLIENTS"] = $Clients }
if ($Hwm) { $RunEnv["PERF_MULTI_HWM"] = $Hwm }
if ($SendHwm) { $RunEnv["PERF_MULTI_SNDHWM"] = $SendHwm }
if ($RecvHwm) { $RunEnv["PERF_MULTI_RCVHWM"] = $RecvHwm }
if ($AllowManualSocketOverrides -eq "1") {
    $RunEnv["PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES"] = "1"
}
if ($ConnectConcurrency) { $RunEnv["PERF_MULTI_CONNECT_CONCURRENCY"] = $ConnectConcurrency }
if ($IoThreads) { $RunEnv["PERF_IO_THREADS"] = $IoThreads }
if ($Build.IsPresent) {
    $RunEnv["PERF_NO_AUTOBUILD"] = "0"
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

$RunArgs = @($PatternCsv)
if ($BuildDir) { $RunArgs += @("--build-dir", $BuildDir) }
$RunArgs += @("--runs", $Runs.ToString(), "--duration", $Duration.ToString())
if ($ResultsDir) { $RunArgs += @("--results-dir", $ResultsDir) }
if ($ResultsTag) { $RunArgs += @("--results-tag", $ResultsTag) }
if ($PinCpu) { $RunArgs += "--pin-cpu" }

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

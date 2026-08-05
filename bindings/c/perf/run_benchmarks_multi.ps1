param(
    [string]$Pattern = "ALL",
    [string]$BuildDir = "",
    [string]$OutputFile = "",
    [int]$Runs = 0,
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
    [string]$SendTimeoutMs = "200",
    [Alias("MultiRcvtimeoMs")]
    [string]$RecvTimeoutMs = "200",
    [Alias("MultiConnectConcurrency")]
    [string]$ConnectConcurrency = "",
    [Alias("MultiTransportTransitionMs")]
    [int]$TransportTransitionMs = 3000,
    [Alias("MultiPatternTransitionMs")]
    [int]$PatternTransitionMs = 3000,
    [Alias("MultiServerReadyTimeoutMs")]
    [int]$ServerReadyTimeoutMs = 10000,
    [Alias("MultiConnectReadyTimeoutMs")]
    [int]$ConnectReadyTimeoutMs = 5000,
    [Alias("MultiMonitorHwm")]
    [int]$MonitorHwm = 1000,
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
  -BuildDir PATH               Build directory.
  -OutputFile PATH             Tee console logs to file.
  -Runs N                      Iterations per configuration (default: 3).
  -Build                       Force clean build (default is reuse-build).
  -ResultsDir PATH             Override result root directory.
  -ResultsTag NAME             Optional tag appended to result filename.
  -IoThreads N                 Set PERF_IO_THREADS.
                               Default multi io-threads are non-stream=2, stream=4.
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
  -MonitorHwm N                Monitor socket HWM (default: 1000).
  -ServerShutdownTimeoutMs N   Server shutdown wait timeout(ms).
  -ServerBindPort N            Server bind port (0=auto).
"@
}

if ($Help) {
    Show-Usage
    exit 0
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

if ($Runs -lt 0) { throw "Runs must be >= 0." }
if ($Runs -eq 0) {
    $Runs = 3
}

$ScriptDir = $PSScriptRoot
$BenchComparisonScript = Join-Path $ScriptDir "run_comparison.py"
if (-not (Test-Path $BenchComparisonScript)) {
    throw "comparison script not found: $BenchComparisonScript"
}

$RunEnv = @{}
$RunEnv["PERF_ALLOW_MULTI"] = "1"
$RunEnv["PERF_POLICY"] = "1"
$RunEnv["PERF_MULTI_DEFAULT_IO_THREADS"] = "2"
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
} elseif ($UseReuseBuild) {
    $RunEnv["PERF_NO_AUTOBUILD"] = "1"
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

$RunArgs = @(
    $PatternCsv,
    "--build-dir", $BuildDir,
    "--runs", $Runs.ToString(),
    "--duration", $Duration.ToString()
)
if ($ResultsDir) { $RunArgs += @("--results-dir", $ResultsDir) }
if ($ResultsTag) { $RunArgs += @("--results-tag", $ResultsTag) }
if ($PinCpu) { $RunArgs += "--pin-cpu" }
if ($MsgSizes) { $RunArgs += @("--msg-sizes", $MsgSizes) }
if ($Transports) { $RunArgs += @("--transports", $Transports) }

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

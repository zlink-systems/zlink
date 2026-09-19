$ErrorActionPreference = "Stop"
. "$PSScriptRoot/../redis-common.ps1"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$CppRoot = Get-ZlinkCppSampleTreeRoot
$BuildDir = if ($env:ZLINK_CPP_BUILD_DIR) { $env:ZLINK_CPP_BUILD_DIR } else { Join-Path $CppRoot "build" }
$BuildConfiguration = if ($env:ZLINK_CPP_BUILD_CONFIGURATION) { $env:ZLINK_CPP_BUILD_CONFIGURATION } else { "Release" }
$CTestBin = if ($env:CTEST_BIN) { $env:CTEST_BIN } else { "ctest" }
$LogDir = Join-Path $ScriptDir "build/sample-logs"
$ConfigDir = Join-Path $LogDir "config"

New-Item -ItemType Directory -Force -Path $LogDir, $ConfigDir | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $LogDir "*.log")

function Find-Binary([string]$Name) {
    $candidates = @(
        (Join-Path $BuildDir $Name),
        (Join-Path $BuildDir "$Name.exe"),
        (Join-Path $BuildDir "$BuildConfiguration/$Name"),
        (Join-Path $BuildDir "$BuildConfiguration/$Name.exe"),
        (Join-Path $BuildDir "linux-ninja-debug/$Name"),
        (Join-Path $BuildDir "linux-ninja-debug/$Name.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }
    throw "Missing executable: $Name. Build C++ samples first or set ZLINK_CPP_BUILD_DIR."
}

$ApiBin = Find-Binary "sample_cpp_framework_bingo_api"
$MatchmakingBin = Find-Binary "sample_cpp_framework_bingo_matchmaking"
$PlayBin = Find-Binary "sample_cpp_framework_bingo_play"
$SessionBin = Find-Binary "sample_cpp_framework_bingo_session"
$ClientBin = Find-Binary "sample_cpp_framework_bingo_client"

$Processes = New-Object System.Collections.Generic.List[System.Diagnostics.Process]
$RedisContainer = $null
$RedisKeyPrefix = if ($env:BINGO_REDIS_KEY_PREFIX) { $env:BINGO_REDIS_KEY_PREFIX } else { "bingo:cpp:${PID}:$([Guid]::NewGuid().ToString('N')):" }

function Print-Logs([int]$Status) {
    if ($Status -eq 0) {
        return
    }
    Get-ChildItem -Path $LogDir -Filter "*.log" -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Host "===== $($_.FullName) ====="
        Get-Content -Path $_.FullName -Tail 200 -ErrorAction SilentlyContinue | ForEach-Object {
            Write-Host $_
        }
    }
}

function Cleanup([int]$Status) {
    for ($i = $Processes.Count - 1; $i -ge 0; $i--) {
        $process = $Processes[$i]
        if ($process.HasExited -and $process.ExitCode -ne 0) {
            Write-Host "cleanup process $($process.Id) exited unexpectedly with status $($process.ExitCode)"
            $Status = 1
            continue
        }
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
    }
    foreach ($process in $Processes) {
        try {
            if (-not $process.WaitForExit(1000)) {
                Write-Host "cleanup process $($process.Id) did not exit after stop"
                $Status = 1
            }
        } catch {
            Write-Host "cleanup process $($process.Id) wait failed: $($_.Exception.Message)"
            $Status = 1
        }
    }
    if ($RedisContainer) {
        Remove-ZlinkSampleRedis $RedisContainer
    }
    Print-Logs $Status
    Close-ZlinkSampleRunDir -RunDir $LogDir -Status $Status -Label "Bingo"
    return $Status
}

function Reserve-Endpoints([int]$Count) {
    return @(Get-ZlinkSamplePorts -Count $Count -Paired |
        ForEach-Object { "127.0.0.1:$_" })
}

function Split-Endpoint([string]$Endpoint) {
    $value = $Endpoint -replace '^tcp://', '' -replace '^http://', ''
    $index = $value.LastIndexOf(':')
    return @{ Host = $value.Substring(0, $index); Port = [int]$value.Substring($index + 1) }
}

function Wait-Endpoint([string]$Name, [string]$Endpoint, [int]$TimeoutSeconds = 60) {
    $parts = Split-Endpoint $Endpoint
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $client = [System.Net.Sockets.TcpClient]::new()
        try {
            $connect = $client.BeginConnect($parts.Host, $parts.Port, $null, $null)
            if ($connect.AsyncWaitHandle.WaitOne(200)) {
                $client.EndConnect($connect)
                return
            }
        } catch {
        } finally {
            $client.Close()
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for $Name at $Endpoint"
}

function Wait-Log([string]$Name, [string]$Path, [string]$Pattern) {
    for ($attempt = 0; $attempt -lt 300; $attempt++) {
        if ((Test-Path $Path) -and (Select-String -Path $Path -SimpleMatch -CaseSensitive -Pattern $Pattern -Quiet)) {
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for $Name evidence in $Path"
}

function Get-LogLineCount([string[]]$Paths, [string]$ExpectedLine) {
    $count = 0
    foreach ($path in $Paths) {
        if (-not (Test-Path $path)) {
            continue
        }
        Get-Content -Path $path | ForEach-Object {
            if ($_.Contains($ExpectedLine)) {
                $count++
            }
        }
    }
    return $count
}

function Wait-LogCount([string]$Name, [string[]]$Paths, [string]$ExpectedLine, [int]$ExpectedCount) {
    $actualCount = 0
    for ($attempt = 0; $attempt -lt 300; $attempt++) {
        $actualCount = Get-LogLineCount $Paths $ExpectedLine
        if ($actualCount -eq $ExpectedCount) {
            return
        }
        if ($actualCount -gt $ExpectedCount) {
            break
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Expected $Name exactly $ExpectedCount time(s), found $actualCount."
}

function Start-Server([string]$Name, [string]$Binary, [string[]]$Arguments) {
    $logPath = Join-Path $LogDir "$Name.log"
    $errorLogPath = Join-Path $LogDir "$Name.err.log"
    $process = Start-Process -FilePath $Binary -ArgumentList $Arguments -RedirectStandardOutput $logPath -RedirectStandardError $errorLogPath -NoNewWindow -PassThru
    [void]$process.Handle
    $Processes.Add($process)
}

function Write-RoleConfig(
    [string]$Name,
    [string]$ApiNode,
    [string]$PlayNode,
    [string]$SessionNode,
    [string]$SessionSpotEndpoint,
    [string]$SessionRouterEndpoint,
    [string]$StreamEndpoint) {
    $topology = @{
        apiChannelEndpoint = $apiAChannelEndpoint
        apiAChannelEndpoint = $apiAChannelEndpoint
        apiBChannelEndpoint = $apiBChannelEndpoint
        playChannelEndpoint = $playAChannelEndpoint
        playAChannelEndpoint = $playAChannelEndpoint
        playBChannelEndpoint = $playBChannelEndpoint
        playARouteEndpoint = $playARouteEndpoint
        playBRouteEndpoint = $playBRouteEndpoint
        apiAPlayRouteEndpoint = $apiAPlayRouteEndpoint
        apiBPlayRouteEndpoint = $apiBPlayRouteEndpoint
        apiAMatchmakingRouteEndpoint = $apiAMatchmakingRouteEndpoint
        apiBMatchmakingRouteEndpoint = $apiBMatchmakingRouteEndpoint
        matchmakingRouteEndpoint = $matchmakingRouteEndpoint
        sessionAPlayRouteEndpoint = $sessionAPlayRouteEndpoint
        sessionBPlayRouteEndpoint = $sessionBPlayRouteEndpoint
        playASpotEndpoint = $playASpotEndpoint
        playBSpotEndpoint = $playBSpotEndpoint
        playASpotRouterEndpoint = $playASpotRouterEndpoint
        playBSpotRouterEndpoint = $playBSpotRouterEndpoint
        sessionSpotEndpoint = $sessionASpotEndpoint
        sessionRouterEndpoint = $sessionARouterEndpoint
        sessionAStreamEndpoint = $sessionAStreamEndpoint
        sessionBStreamEndpoint = $sessionBStreamEndpoint
        logDir = $LogDir
        redisEndpoint = $redisEndpoint
        redisKeyPrefix = $RedisKeyPrefix
    }
    if ($ApiNode) { $topology.apiNode = $ApiNode }
    if ($PlayNode) { $topology.playNode = $PlayNode }
    if ($SessionNode) { $topology.sessionNode = $SessionNode }
    if ($SessionSpotEndpoint) { $topology.sessionSpotEndpoint = $SessionSpotEndpoint }
    if ($SessionRouterEndpoint) { $topology.sessionRouterEndpoint = $SessionRouterEndpoint }
    if ($StreamEndpoint) { $topology.streamEndpoint = $StreamEndpoint }

    $configuration = @{ sample = @{ host = @{ keepRunning = $true }; topology = $topology } }
    $configuration | ConvertTo-Json -Depth 5 | Set-Content -Path (Join-Path $ConfigDir "$Name.json") -Encoding utf8
}

function Invoke-Checked([string]$FilePath, [string[]]$Arguments) {
    # The verdict is the exit code. Under ErrorActionPreference Stop a native
    # command writing progress to stderr would terminate the runner before the
    # exit code is ever read, so the call itself must not raise.
    $previous = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        & $FilePath @Arguments
    } finally {
        $ErrorActionPreference = $previous
    }
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE"
    }
}

$Status = 1
try {
    Invoke-Checked $CTestBin @(
        "--test-dir", $BuildDir,
        "-C", $BuildConfiguration,
        "-R", "test_cpp_framework_sample_parity|zlink_cpp_framework_mesh_node_vertical_test|test_cpp_framework_actor_gateway",
        "--output-on-failure"
    )

    $ports = Reserve-Endpoints 24
    $apiAChannelEndpoint = if ($env:BINGO_API_A_CHANNEL_ENDPOINT) { $env:BINGO_API_A_CHANNEL_ENDPOINT } else { "tcp://$($ports[2])" }
    $playAChannelEndpoint = if ($env:BINGO_PLAY_A_CHANNEL_ENDPOINT) { $env:BINGO_PLAY_A_CHANNEL_ENDPOINT } else { "tcp://$($ports[3])" }
    $sessionASpotEndpoint = if ($env:BINGO_SESSION_A_SPOT_ENDPOINT) { $env:BINGO_SESSION_A_SPOT_ENDPOINT } else { "tcp://$($ports[4])" }
    $sessionARouterEndpoint = if ($env:BINGO_SESSION_A_ROUTER_ENDPOINT) { $env:BINGO_SESSION_A_ROUTER_ENDPOINT } else { "tcp://$($ports[5])" }
    $sessionBSpotEndpoint = if ($env:BINGO_SESSION_B_SPOT_ENDPOINT) { $env:BINGO_SESSION_B_SPOT_ENDPOINT } else { "tcp://$($ports[6])" }
    $sessionBRouterEndpoint = if ($env:BINGO_SESSION_B_ROUTER_ENDPOINT) { $env:BINGO_SESSION_B_ROUTER_ENDPOINT } else { "tcp://$($ports[7])" }
    $playBChannelEndpoint = if ($env:BINGO_PLAY_B_CHANNEL_ENDPOINT) { $env:BINGO_PLAY_B_CHANNEL_ENDPOINT } else { "tcp://$($ports[8])" }
    $playASpotEndpoint = if ($env:BINGO_PLAY_A_SPOT_ENDPOINT) { $env:BINGO_PLAY_A_SPOT_ENDPOINT } else { "tcp://$($ports[9])" }
    $playASpotRouterEndpoint = if ($env:BINGO_PLAY_A_SPOT_ROUTER_ENDPOINT) { $env:BINGO_PLAY_A_SPOT_ROUTER_ENDPOINT } else { "tcp://$($ports[10])" }
    $sessionAStreamEndpoint = if ($env:BINGO_SESSION_A_STREAM_ENDPOINT) { $env:BINGO_SESSION_A_STREAM_ENDPOINT } else { "tcp://$($ports[11])" }
    $sessionBStreamEndpoint = if ($env:BINGO_SESSION_B_STREAM_ENDPOINT) { $env:BINGO_SESSION_B_STREAM_ENDPOINT } else { "tcp://$($ports[12])" }
    $playBSpotEndpoint = if ($env:BINGO_PLAY_B_SPOT_ENDPOINT) { $env:BINGO_PLAY_B_SPOT_ENDPOINT } else { "tcp://$($ports[13])" }
    $playBSpotRouterEndpoint = if ($env:BINGO_PLAY_B_SPOT_ROUTER_ENDPOINT) { $env:BINGO_PLAY_B_SPOT_ROUTER_ENDPOINT } else { "tcp://$($ports[14])" }
    $apiBChannelEndpoint = if ($env:BINGO_API_B_CHANNEL_ENDPOINT) { $env:BINGO_API_B_CHANNEL_ENDPOINT } else { "tcp://$($ports[15])" }
    $playARouteEndpoint = if ($env:BINGO_PLAY_A_ROUTE_ENDPOINT) { $env:BINGO_PLAY_A_ROUTE_ENDPOINT } else { "tcp://$($ports[0])" }
    $playBRouteEndpoint = if ($env:BINGO_PLAY_B_ROUTE_ENDPOINT) { $env:BINGO_PLAY_B_ROUTE_ENDPOINT } else { "tcp://$($ports[1])" }
    $apiAPlayRouteEndpoint = if ($env:BINGO_API_A_PLAY_ROUTE_ENDPOINT) { $env:BINGO_API_A_PLAY_ROUTE_ENDPOINT } else { "tcp://$($ports[16])" }
    $apiBPlayRouteEndpoint = if ($env:BINGO_API_B_PLAY_ROUTE_ENDPOINT) { $env:BINGO_API_B_PLAY_ROUTE_ENDPOINT } else { "tcp://$($ports[17])" }
    $apiAMatchmakingRouteEndpoint = if ($env:BINGO_API_A_MATCHMAKING_ROUTE_ENDPOINT) { $env:BINGO_API_A_MATCHMAKING_ROUTE_ENDPOINT } else { "tcp://$($ports[18])" }
    $apiBMatchmakingRouteEndpoint = if ($env:BINGO_API_B_MATCHMAKING_ROUTE_ENDPOINT) { $env:BINGO_API_B_MATCHMAKING_ROUTE_ENDPOINT } else { "tcp://$($ports[19])" }
    $matchmakingRouteEndpoint = if ($env:BINGO_MATCHMAKING_ROUTE_ENDPOINT) { $env:BINGO_MATCHMAKING_ROUTE_ENDPOINT } else { "tcp://$($ports[20])" }
    $sessionAPlayRouteEndpoint = if ($env:BINGO_SESSION_A_PLAY_ROUTE_ENDPOINT) { $env:BINGO_SESSION_A_PLAY_ROUTE_ENDPOINT } else { "tcp://$($ports[21])" }
    $sessionBPlayRouteEndpoint = if ($env:BINGO_SESSION_B_PLAY_ROUTE_ENDPOINT) { $env:BINGO_SESSION_B_PLAY_ROUTE_ENDPOINT } else { "tcp://$($ports[22])" }

    $redis = Start-ZlinkSampleRedis "zlink-redis-cpp-sample-bingo"
    $RedisContainer = $redis.ContainerId
    $redisEndpoint = $redis.Endpoint
    Wait-Endpoint "redis" "tcp://$redisEndpoint"

    Write-RoleConfig "matchmaking"
    Write-RoleConfig "api-a" -ApiNode "a"
    Write-RoleConfig "api-b" -ApiNode "b"
    Write-RoleConfig "session-a" -SessionNode "a" -SessionSpotEndpoint $sessionASpotEndpoint -SessionRouterEndpoint $sessionARouterEndpoint -StreamEndpoint $sessionAStreamEndpoint
    Write-RoleConfig "session-b" -SessionNode "b" -SessionSpotEndpoint $sessionBSpotEndpoint -SessionRouterEndpoint $sessionBRouterEndpoint -StreamEndpoint $sessionBStreamEndpoint
    Write-RoleConfig "play-a" -PlayNode "a"
    Write-RoleConfig "play-b" -PlayNode "b"

    Start-Server "matchmaking" $MatchmakingBin @("--config=$(Join-Path $ConfigDir 'matchmaking.json')")
    Wait-Endpoint "matchmaking" $matchmakingRouteEndpoint

    Start-Server "api-a" $ApiBin @("--config=$(Join-Path $ConfigDir 'api-a.json')")
    Wait-Endpoint "api-a" $apiAChannelEndpoint
    Wait-Endpoint "api-a-play-route" $apiAPlayRouteEndpoint
    Wait-Endpoint "api-a-matchmaking-route" $apiAMatchmakingRouteEndpoint
    Start-Server "api-b" $ApiBin @("--config=$(Join-Path $ConfigDir 'api-b.json')")
    Wait-Endpoint "api-b" $apiBChannelEndpoint
    Wait-Endpoint "api-b-play-route" $apiBPlayRouteEndpoint
    Wait-Endpoint "api-b-matchmaking-route" $apiBMatchmakingRouteEndpoint

    Start-Server "session-a" $SessionBin @("--config=$(Join-Path $ConfigDir 'session-a.json')")
    Wait-Endpoint "session-a-stream" $sessionAStreamEndpoint
    Wait-Endpoint "session-a-play-route" $sessionAPlayRouteEndpoint

    Start-Server "session-b" $SessionBin @("--config=$(Join-Path $ConfigDir 'session-b.json')")
    Wait-Endpoint "session-b-stream" $sessionBStreamEndpoint
    Wait-Endpoint "session-b-play-route" $sessionBPlayRouteEndpoint

    Start-Server "play-a" $PlayBin @("--config=$(Join-Path $ConfigDir 'play-a.json')")
    Wait-Endpoint "play-a-spot-router" $playASpotRouterEndpoint
    Start-Server "play-b" $PlayBin @("--config=$(Join-Path $ConfigDir 'play-b.json')")
    Wait-Endpoint "play-b-spot-router" $playBSpotRouterEndpoint

    Wait-Log "play-a peer route readiness" (Join-Path $LogDir "play-a.log") "bingo-ready kind=peer-route node=play-a peer=play-b"
    Wait-Log "play-b peer route readiness" (Join-Path $LogDir "play-b.log") "bingo-ready kind=peer-route node=play-b peer=play-a"
    Wait-Log "api-a matchmaking route readiness" (Join-Path $LogDir "api-a.log") "bingo-ready kind=mesh-route node=api-a mesh=matchmaking"
    Wait-Log "api-a room route readiness" (Join-Path $LogDir "api-a.log") "bingo-ready kind=mesh-route node=api-a mesh=room"
    Wait-Log "api-b matchmaking route readiness" (Join-Path $LogDir "api-b.log") "bingo-ready kind=mesh-route node=api-b mesh=matchmaking"
    Wait-Log "api-b room route readiness" (Join-Path $LogDir "api-b.log") "bingo-ready kind=mesh-route node=api-b mesh=room"
    Wait-Log "session-a room route readiness" (Join-Path $LogDir "session-a.log") "bingo-ready kind=mesh-route node=session-a mesh=room"
    Wait-Log "session-b room route readiness" (Join-Path $LogDir "session-b.log") "bingo-ready kind=mesh-route node=session-b mesh=room"

    $clientLog = Join-Path $LogDir "client.log"
    Invoke-Checked $ClientBin @(
        "--session-a-stream-endpoint", $sessionAStreamEndpoint,
        "--session-b-stream-endpoint", $sessionBStreamEndpoint
    ) *> $clientLog

    if (-not (Select-String -Path $clientLog -Pattern "bingo=completed" -Quiet)) {
        throw "Bingo C++ client did not write completion marker."
    }
    if (-not (Select-String -Path $clientLog -Pattern "stream-result sample=Bingo client=player1 operation=authenticate" -Quiet)) {
        throw "Bingo C++ client did not write stream response handling evidence."
    }
    if (-not (Select-String -Path $clientLog -Pattern "stream-handler sample=Bingo client=player1 message=PlayerJoinedNotify" -Quiet)) {
        throw "Bingo C++ client did not write stream push handler evidence."
    }
    $playLogs = @((Join-Path $LogDir "play-a.log"), (Join-Path $LogDir "play-b.log"))
    $sessionLogs = @((Join-Path $LogDir "session-a.log"), (Join-Path $LogDir "session-b.log"))

    Wait-LogCount "player-1 record fetch" $playLogs "bingo-record fetched actor=player-1 wins=0 losses=0" 1
    Wait-LogCount "player-2 record fetch" $playLogs "bingo-record fetched actor=player-2 wins=0 losses=0" 1
    Wait-LogCount "player-1 record report" $playLogs "bingo-record reported actor=player-1 wins=1 losses=0" 1
    Wait-LogCount "player-2 record report" $playLogs "bingo-record reported actor=player-2 wins=0 losses=1" 1
    foreach ($actorId in @("player-1", "player-2", "observer")) {
        Wait-LogCount "$actorId room leave" $playLogs "bingo-lifecycle room-leave actor=$actorId" 1
        Wait-LogCount "$actorId Entry Spot leave" $playLogs "bingo-lifecycle entry-leave actor=$actorId" 1
    }
    Wait-LogCount "player-1 Entry Spot destroy completion" $playLogs "bingo-lifecycle entry-destroy-complete actor=player-1" 1
    Wait-LogCount "player-2 Entry Spot destroy completion" $playLogs "bingo-lifecycle entry-destroy-complete actor=player-2" 1
    Wait-LogCount "player-1 session disconnect" $sessionLogs "bingo-lifecycle session-disconnect actor=player-1 destroy=false" 1
    Wait-LogCount "player-2 session disconnect" $sessionLogs "bingo-lifecycle session-disconnect actor=player-2 destroy=false" 1
    Wait-LogCount "observer record report" $playLogs "bingo-record reported actor=observer" 0
    Wait-LogCount "observer Entry Spot destroy completion" $playLogs "bingo-lifecycle entry-destroy-complete actor=observer" 0

    $Status = 0
} finally {
    $Status = Cleanup $Status
}

if ($Status -ne 0) {
    exit $Status
}
Write-Host "bingo full client/server self-check completed"
Write-Host "bingo-placement=completed"

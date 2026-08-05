$ErrorActionPreference = "Stop"
. "$PSScriptRoot/../redis-common.ps1"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$CppRoot = Resolve-Path (Join-Path $ScriptDir "../..")
$env:TICTACTOE_LOG_DIR = if ($env:TICTACTOE_LOG_DIR) { $env:TICTACTOE_LOG_DIR } else { Join-Path $ScriptDir "logs" }
New-Item -ItemType Directory -Force -Path $env:TICTACTOE_LOG_DIR | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $env:TICTACTOE_LOG_DIR "*.log")

$BuildDir = if ($env:ZLINK_CPP_BUILD_DIR) { $env:ZLINK_CPP_BUILD_DIR } else { Join-Path $CppRoot "build" }
$BinDir = $BuildDir
if (-not (Test-Path (Join-Path $BinDir "sample_cpp_framework_tictactoe_play.exe")) -and
    (Test-Path (Join-Path $BinDir "linux-ninja-debug/sample_cpp_framework_tictactoe_play.exe"))) {
    $BinDir = Join-Path $BinDir "linux-ninja-debug"
}

$PlayBin = Join-Path $BinDir "sample_cpp_framework_tictactoe_play.exe"
$ApiBin = Join-Path $BinDir "sample_cpp_framework_tictactoe_api.exe"
$ClientBin = Join-Path $BinDir "sample_cpp_framework_tictactoe_client.exe"
$CTestBin = if ($env:CTEST_BIN) { $env:CTEST_BIN } else { "ctest" }

foreach ($Binary in @($PlayBin, $ApiBin, $ClientBin)) {
    if (-not (Test-Path $Binary)) {
        throw "Missing executable: $Binary. Build C++ samples first or set ZLINK_CPP_BUILD_DIR."
    }
}

function Reserve-Ports([int]$Count) {
    if ($env:TICTACTOE_CPP_BASE_PORT) {
        $ports = New-Object System.Collections.Generic.List[int]
        $basePort = [int]$env:TICTACTOE_CPP_BASE_PORT
        for ($i = 1; $i -le $Count; $i++) {
            $ports.Add($basePort + $i)
        }
        return $ports.ToArray()
    }

    $listeners = New-Object System.Collections.Generic.List[System.Net.Sockets.TcpListener]
    $ports = New-Object System.Collections.Generic.List[int]
    try {
        while ($ports.Count -lt $Count) {
            $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Parse("127.0.0.1"), 0)
            $listener.Start()
            $listeners.Add($listener)
            $ports.Add([int]$listener.LocalEndpoint.Port)
        }
        return $ports.ToArray()
    } finally {
        foreach ($listener in $listeners) {
            $listener.Stop()
        }
    }
}

function Get-EndpointParts([string]$Endpoint) {
    $value = $Endpoint -replace '^tcp://', '' -replace '^http://', '' -replace '^redis://', ''
    $index = $value.LastIndexOf(':')
    return @{ Host = $value.Substring(0, $index); Port = [int]$value.Substring($index + 1) }
}

function Wait-Port([string]$Name, [string]$Endpoint, [int]$TimeoutSeconds = 30) {
    $parts = Get-EndpointParts $Endpoint
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

function Wait-Grep([string]$Pattern, [string]$Path) {
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Select-String -Path $Path -Pattern $Pattern -Quiet -ErrorAction SilentlyContinue) {
            return
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not (Select-String -Path $Path -Pattern $Pattern -Quiet -ErrorAction SilentlyContinue)) {
        throw "Pattern '$Pattern' was not found in $Path"
    }
}

function Wait-RouteReady([string]$BaseUrl, [string]$TargetRid, [int]$TimeoutSeconds = 30) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $uri = "$BaseUrl/ready?targetRid=$TargetRid"
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            $response = Invoke-WebRequest -UseBasicParsing -Uri $uri -TimeoutSec 1
            if ($response.StatusCode -eq 200) {
                return
            }
        } catch {
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for API route peer $TargetRid"
}

function Start-Server([string]$Name, [string]$Binary, [string[]]$Arguments) {
    $stdout = Join-Path $LogDir "$Name.log"
    $stderr = Join-Path $LogDir "$Name.err.log"
    $process = Start-Process -FilePath $Binary -ArgumentList $Arguments -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    $script:Processes.Add($process)
}

function Print-Logs() {
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
        & docker rm -fv $RedisContainer 2>$null | Out-Null
    }
    if ($Status -ne 0) {
        Print-Logs
    }
    if ($env:TICTACTOE_CPP_KEEP_RUN_DIR -eq "1") {
        Write-Host "runDir=$LogDir"
    } else {
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $LogDir
    }
    return $Status
}

& $CTestBin --test-dir $BuildDir `
    -R "test_cpp_framework_sample_parity|zlink_cpp_framework_mesh_node_vertical_test|test_cpp_framework_actor_gateway|sample_smoke_sample_cpp_framework_tictactoe_(play|api)" `
    --output-on-failure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$ports = Reserve-Ports 17
$ApiAEndpoint = "tcp://127.0.0.1:$($ports[0])"
$ApiBEndpoint = "tcp://127.0.0.1:$($ports[1])"
$ApiAHttpEndpoint = "http://127.0.0.1:$($ports[2])"
$ApiBHttpEndpoint = "http://127.0.0.1:$($ports[3])"
$PlayAEndpoint = "tcp://127.0.0.1:$($ports[4])"
$PlayBEndpoint = "tcp://127.0.0.1:$($ports[5])"
$PlayAStreamEndpoint = "tcp://127.0.0.1:$($ports[6])"
$PlayBStreamEndpoint = "tcp://127.0.0.1:$($ports[7])"
$PlayASpotEndpoint = "tcp://127.0.0.1:$($ports[8])"
$PlayBSpotEndpoint = "tcp://127.0.0.1:$($ports[9])"
$PlayASpotRouterEndpoint = "tcp://127.0.0.1:$($ports[10])"
$PlayBSpotRouterEndpoint = "tcp://127.0.0.1:$($ports[11])"
$PlayARouteEndpoint = "tcp://127.0.0.1:$($ports[12])"
$PlayBRouteEndpoint = "tcp://127.0.0.1:$($ports[13])"
$ApiARouteEndpoint = "tcp://127.0.0.1:$($ports[14])"
$ApiBRouteEndpoint = "tcp://127.0.0.1:$($ports[15])"
$RedisPort = $ports[16]

$LogDir = Join-Path ([System.IO.Path]::GetTempPath()) ([System.Guid]::NewGuid().ToString())
New-Item -ItemType Directory -Path $LogDir | Out-Null
$Processes = New-Object System.Collections.Generic.List[System.Diagnostics.Process]
$RedisContainer = $null
$RedisKeyPrefix = if ($env:TICTACTOE_CPP_REDIS_KEY_PREFIX) { $env:TICTACTOE_CPP_REDIS_KEY_PREFIX } else { "zlink:tictactoe-cpp:${PID}:$([Guid]::NewGuid().ToString('N')):room:" }

$Status = 1
try {
    $redis = Start-ZlinkSampleRedis "zlink-redis-cpp-sample-tictactoe" "redis:7-alpine"
    $RedisContainer = $redis.ContainerId
    $RedisEndpoint = $redis.Endpoint
    Wait-Port "redis" $RedisEndpoint

    $topologyArgs = @(
        "--sample.topology.apiEndpoint=$ApiAEndpoint",
        "--sample.topology.apiAEndpoint=$ApiAEndpoint",
        "--sample.topology.apiBEndpoint=$ApiBEndpoint",
        "--sample.topology.apiHttpEndpoint=$ApiAHttpEndpoint",
        "--sample.topology.apiAHttpEndpoint=$ApiAHttpEndpoint",
        "--sample.topology.apiBHttpEndpoint=$ApiBHttpEndpoint",
        "--sample.topology.playEndpoint=$PlayAEndpoint",
        "--sample.topology.playAEndpoint=$PlayAEndpoint",
        "--sample.topology.playBEndpoint=$PlayBEndpoint",
        "--sample.topology.playARouteEndpoint=$PlayARouteEndpoint",
        "--sample.topology.playBRouteEndpoint=$PlayBRouteEndpoint",
        "--sample.topology.apiARouteEndpoint=$ApiARouteEndpoint",
        "--sample.topology.apiBRouteEndpoint=$ApiBRouteEndpoint",
        "--sample.topology.playASpotEndpoint=$PlayASpotEndpoint",
        "--sample.topology.playBSpotEndpoint=$PlayBSpotEndpoint",
        "--sample.topology.playASpotRouterEndpoint=$PlayASpotRouterEndpoint",
        "--sample.topology.playBSpotRouterEndpoint=$PlayBSpotRouterEndpoint",
        "--sample.topology.playAStreamEndpoint=$PlayAStreamEndpoint",
        "--sample.topology.playBStreamEndpoint=$PlayBStreamEndpoint",
        "--sample.topology.redisEndpoint=$RedisEndpoint",
        "--sample.topology.redisKeyPrefix=$RedisKeyPrefix"
    )
    $serverArgs = @("--sample.host.keepRunning", "true") + $topologyArgs

    Start-Server "api-a" $ApiBin ($serverArgs + @("--sample.topology.apiNode=a"))
    Wait-Port "api-a-channel" $ApiAEndpoint
    Wait-Port "api-a-http" $ApiAHttpEndpoint
    Wait-Port "api-a-route" $ApiARouteEndpoint

    Start-Server "api-b" $ApiBin ($serverArgs + @("--sample.topology.apiNode=b"))
    Wait-Port "api-b-channel" $ApiBEndpoint
    Wait-Port "api-b-http" $ApiBHttpEndpoint
    Wait-Port "api-b-route" $ApiBRouteEndpoint

    Start-Server "play-a" $PlayBin ($serverArgs + @("--sample.topology.playNode=a"))
    Wait-Port "play-a-channel" $PlayAEndpoint
    Wait-Port "play-a-stream" $PlayAStreamEndpoint
    Wait-Port "play-a-spot-router" $PlayASpotRouterEndpoint
    Wait-Port "play-a-spot-pub" $PlayASpotEndpoint

    Start-Server "play-b" $PlayBin ($serverArgs + @("--sample.topology.playNode=b"))
    Wait-Port "play-b-channel" $PlayBEndpoint
    Wait-Port "play-b-stream" $PlayBStreamEndpoint
    Wait-Port "play-b-spot-router" $PlayBSpotRouterEndpoint
    Wait-Port "play-b-spot-pub" $PlayBSpotEndpoint
    Wait-RouteReady $ApiAHttpEndpoint "tictactoe-play-a"
    Wait-RouteReady $ApiAHttpEndpoint "tictactoe-play-b"

    $clientLog = Join-Path $LogDir "client.log"
    & $ClientBin --api-http-endpoint $ApiAHttpEndpoint *> $clientLog
    if ($LASTEXITCODE -ne 0) {
        Print-Logs
        exit $LASTEXITCODE
    }

    Wait-Grep "observer-connected endpoint=tcp://127.0.0.1:" $clientLog
    Wait-Grep "observer-subscription=verified subscribed=true" $clientLog
    Wait-Grep "observer-win-milestone=verified actor=player-x wins=100" $clientLog
    Wait-Grep "tictactoe completed" $clientLog
    Wait-Grep "tictactoe=completed" $clientLog
    Wait-Grep "actor: LeaveGameMsg completed. actor=player-x" (Join-Path $LogDir "play-*.log")
    Wait-Grep "actor: LeaveGameMsg completed. actor=player-o" (Join-Path $LogDir "play-*.log")
    Wait-Grep "entry spot: actor destroy completed. actor=player-x" (Join-Path $LogDir "play-*.log")
    Wait-Grep "entry spot: actor destroy completed. actor=player-o" (Join-Path $LogDir "play-*.log")
    if (-not (Select-String -Path (Join-Path $env:TICTACTOE_LOG_DIR "*.log") -Pattern "packet=LeaveGameMsg" -Quiet)) {
        throw "TicTacToe C++ sample logs did not contain LeaveGameMsg evidence."
    }
    if (-not (Select-String -Path (Join-Path $env:TICTACTOE_LOG_DIR "*.log") -Pattern "message flow" -Quiet)) {
        throw "TicTacToe C++ sample logs did not contain message-flow evidence."
    }
    $Status = 0
} finally {
    $Status = Cleanup $Status
}

if ($Status -ne 0) {
    exit $Status
}
Write-Host "PASS TicTacToe.Cpp"
Write-Host "tictactoe full client/server self-check completed"

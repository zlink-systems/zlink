$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptDir "../sample_runner.ps1")

$RunDir = New-SampleRunDirectory "bingo-dotnet"
$RunId = "$PID-$([Guid]::NewGuid().ToString('N'))"
$LogDir = Join-Path $RunDir "logs"
$SampleLogDir = Join-Path $RunDir "sample-logs"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
New-Item -ItemType Directory -Force -Path $SampleLogDir | Out-Null
$BINGO_LOG_DIR = $SampleLogDir
$RedisContainer = $null
$RunSucceeded = $false

function Set-DefaultValue {
    param([string]$Name, [string]$Value)
    if (-not (Get-Variable -Name $Name -Scope Script -ErrorAction SilentlyContinue)) {
        Set-Variable -Name $Name -Value $Value -Scope Script
    }
}

function Wait-LogCount {
    param(
        [Parameter(Mandatory = $true)][string[]]$Path,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][int]$Expected,
        [int]$Attempts = 300
    )

    $count = 0
    for ($i = 0; $i -lt $Attempts; $i++) {
        $existingPaths = @($Path | Where-Object { Test-Path $_ })
        $count = if ($existingPaths.Count -eq 0) {
            0
        }
        else {
            @(Select-String -Path $existingPaths -Pattern $Pattern).Count
        }
        if ($count -eq $Expected) {
            return
        }
        if ($count -gt $Expected) {
            break
        }
        Start-Sleep -Milliseconds 100
    }

    throw "Expected $Expected matches for '$Pattern' in $Path, found $count."
}

try {
    $BINGO_REDIS_KEY_PREFIX = "bingo:dotnet:${RunId}:"

    $basePort = if ($BINGO_BASE_PORT) { [int]$BINGO_BASE_PORT } else { 0 }
    $ports = New-SamplePorts -Count 11 -BasePort $basePort

    Set-DefaultValue "BINGO_API_A_MESH_ENDPOINT" "tcp://127.0.0.1:$($ports[0])"
    Set-DefaultValue "BINGO_API_B_MESH_ENDPOINT" "tcp://127.0.0.1:$($ports[1])"
    Set-DefaultValue "BINGO_PLAY_A_MESH_ENDPOINT" "tcp://127.0.0.1:$($ports[2])"
    Set-DefaultValue "BINGO_PLAY_B_MESH_ENDPOINT" "tcp://127.0.0.1:$($ports[3])"
    Set-DefaultValue "BINGO_SESSION_A_MESH_ENDPOINT" "tcp://127.0.0.1:$($ports[4])"
    Set-DefaultValue "BINGO_SESSION_B_MESH_ENDPOINT" "tcp://127.0.0.1:$($ports[5])"
    Set-DefaultValue "BINGO_SESSION_A_STREAM_ENDPOINT" "tcp://127.0.0.1:$($ports[6])"
    Set-DefaultValue "BINGO_SESSION_B_STREAM_ENDPOINT" "tcp://127.0.0.1:$($ports[7])"
    Set-DefaultValue "BINGO_API_A_MATCHMAKING_ENDPOINT" "tcp://127.0.0.1:$($ports[8])"
    Set-DefaultValue "BINGO_API_B_MATCHMAKING_ENDPOINT" "tcp://127.0.0.1:$($ports[9])"
    Set-DefaultValue "BINGO_MATCHMAKING_MESH_ENDPOINT" "tcp://127.0.0.1:$($ports[10])"
    $redis = Start-SampleRedisContainer "zlink-bingo-dotnet-redis"
    $RedisContainer = $redis.ContainerId
    $BINGO_REDIS_ENDPOINT = $redis.Endpoint
    Wait-SampleTcpEndpoint "redis" "tcp://$BINGO_REDIS_ENDPOINT"

    $common = @{
        LogDirectory = $SampleLogDir
        RedisEndpoint = $BINGO_REDIS_ENDPOINT
        RedisKeyPrefix = $BINGO_REDIS_KEY_PREFIX
    }
    $roles = @{
        "api-a" = $common + @{ NodeName = "a"; MeshEndpoint = $BINGO_API_A_MESH_ENDPOINT; MatchmakingMeshEndpoint = $BINGO_API_A_MATCHMAKING_ENDPOINT }
        "api-b" = $common + @{ NodeName = "b"; MeshEndpoint = $BINGO_API_B_MESH_ENDPOINT; MatchmakingMeshEndpoint = $BINGO_API_B_MATCHMAKING_ENDPOINT }
        "matchmaking" = $common + @{ NodeName = "matchmaking"; MeshEndpoint = $BINGO_MATCHMAKING_MESH_ENDPOINT }
        "play-a" = $common + @{ NodeName = "a"; MeshEndpoint = $BINGO_PLAY_A_MESH_ENDPOINT }
        "play-b" = $common + @{ NodeName = "b"; MeshEndpoint = $BINGO_PLAY_B_MESH_ENDPOINT }
        "session-a" = $common + @{ NodeName = "a"; MeshEndpoint = $BINGO_SESSION_A_MESH_ENDPOINT; StreamEndpoint = $BINGO_SESSION_A_STREAM_ENDPOINT }
        "session-b" = $common + @{ NodeName = "b"; MeshEndpoint = $BINGO_SESSION_B_MESH_ENDPOINT; StreamEndpoint = $BINGO_SESSION_B_STREAM_ENDPOINT }
    }
    $configFiles = @{}
    foreach ($role in $roles.Keys) {
        $path = Join-Path $RunDir "appsettings.$role.json"
        @{ Sample = $roles[$role] } | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -Path $path
        $configFiles[$role] = $path
    }
    $clientPath = Join-Path $RunDir "appsettings.client.json"
    @{ Client = @{
        LogDirectory = $SampleLogDir
        SessionAStreamEndpoint = $BINGO_SESSION_A_STREAM_ENDPOINT
        SessionBStreamEndpoint = $BINGO_SESSION_B_STREAM_ENDPOINT
    } } | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -Path $clientPath
    $configFiles["client"] = $clientPath

    Invoke-SampleDotnetBuild (Join-Path $ScriptDir "Bingo.csproj")

    Start-SampleDotnetAssembly -Name "play-b" -Project (Join-Path $ScriptDir "Server/Play/Bingo.Server.Play.csproj") -LogDirectory $LogDir -Arguments @("--config", $configFiles["play-b"]) | Out-Null
    Wait-SampleTcpEndpoint "play-b-mesh" $BINGO_PLAY_B_MESH_ENDPOINT
    Start-SampleDotnetAssembly -Name "play-a" -Project (Join-Path $ScriptDir "Server/Play/Bingo.Server.Play.csproj") -LogDirectory $LogDir -Arguments @("--config", $configFiles["play-a"]) | Out-Null
    Wait-SampleTcpEndpoint "play-a-mesh" $BINGO_PLAY_A_MESH_ENDPOINT

    Start-SampleDotnetAssembly -Name "matchmaking" -Project (Join-Path $ScriptDir "Server/Matchmaking/Bingo.Server.Matchmaking.csproj") -LogDirectory $LogDir -Arguments @("--config", $configFiles["matchmaking"]) | Out-Null
    Wait-SampleTcpEndpoint "matchmaking-mesh" $BINGO_MATCHMAKING_MESH_ENDPOINT

    Start-SampleDotnetAssembly -Name "api-a" -Project (Join-Path $ScriptDir "Server/Api/Bingo.Server.Api.csproj") -LogDirectory $LogDir -Arguments @("--config", $configFiles["api-a"]) | Out-Null
    Wait-SampleTcpEndpoint "api-a-mesh" $BINGO_API_A_MESH_ENDPOINT
    Wait-SampleTcpEndpoint "api-a-matchmaking" $BINGO_API_A_MATCHMAKING_ENDPOINT
    Start-SampleDotnetAssembly -Name "api-b" -Project (Join-Path $ScriptDir "Server/Api/Bingo.Server.Api.csproj") -LogDirectory $LogDir -Arguments @("--config", $configFiles["api-b"]) | Out-Null
    Wait-SampleTcpEndpoint "api-b-mesh" $BINGO_API_B_MESH_ENDPOINT
    Wait-SampleTcpEndpoint "api-b-matchmaking" $BINGO_API_B_MATCHMAKING_ENDPOINT

    Start-SampleDotnetAssembly -Name "session-a" -Project (Join-Path $ScriptDir "Server/Session/Bingo.Server.Session.csproj") -LogDirectory $LogDir -Arguments @("--config", $configFiles["session-a"]) | Out-Null
    Wait-SampleTcpEndpoint "session-a-mesh" $BINGO_SESSION_A_MESH_ENDPOINT
    Wait-SampleTcpEndpoint "session-a-stream" $BINGO_SESSION_A_STREAM_ENDPOINT
    Start-SampleDotnetAssembly -Name "session-b" -Project (Join-Path $ScriptDir "Server/Session/Bingo.Server.Session.csproj") -LogDirectory $LogDir -Arguments @("--config", $configFiles["session-b"]) | Out-Null
    Wait-SampleTcpEndpoint "session-b-mesh" $BINGO_SESSION_B_MESH_ENDPOINT
    Wait-SampleTcpEndpoint "session-b-stream" $BINGO_SESSION_B_STREAM_ENDPOINT

    $playA = Join-Path $LogDir "play-a.out.log"
    $playB = Join-Path $LogDir "play-b.out.log"
    $apiA = Join-Path $LogDir "api-a.out.log"
    $apiB = Join-Path $LogDir "api-b.out.log"
    $sessionA = Join-Path $LogDir "session-a.out.log"
    $sessionB = Join-Path $LogDir "session-b.out.log"
    Wait-LogCount -Path $playA -Pattern "bingo-ready kind=peer-route node=play-a peer=play-b" -Expected 1
    Wait-LogCount -Path $playB -Pattern "bingo-ready kind=peer-route node=play-b peer=play-a" -Expected 1
    Wait-LogCount -Path $apiA -Pattern "bingo-ready kind=mesh-route node=api-a mesh=matchmaking" -Expected 1
    Wait-LogCount -Path $apiA -Pattern "bingo-ready kind=mesh-route node=api-a mesh=room" -Expected 1
    Wait-LogCount -Path $apiB -Pattern "bingo-ready kind=mesh-route node=api-b mesh=matchmaking" -Expected 1
    Wait-LogCount -Path $apiB -Pattern "bingo-ready kind=mesh-route node=api-b mesh=room" -Expected 1
    Wait-LogCount -Path $sessionA -Pattern "bingo-ready kind=mesh-route node=session-a mesh=room" -Expected 1
    Wait-LogCount -Path $sessionB -Pattern "bingo-ready kind=mesh-route node=session-b mesh=room" -Expected 1

    $clientLog = Join-Path $LogDir "client.log"
    Invoke-SampleDotnetRun -Project (Join-Path $ScriptDir "Client/Bingo.Client.csproj") -Arguments @("--config", $configFiles["client"]) *> $clientLog
    if (-not (Select-String -Path $clientLog -Pattern "bingo=completed" -Quiet)) {
        throw "Bingo client did not complete."
    }
    if (-not (Select-String -Path $clientLog -Pattern "stream-inbound sample=Bingo" -Quiet)) {
        throw "Bingo client did not write stream-inbound marker."
    }
    if (-not (Select-String -Path $clientLog -Pattern "stream-inbound sample=Bingo .* seq=[0-9]" -Quiet)) {
        throw "Bingo client did not write sequenced stream-inbound response marker."
    }
    if (-not (Select-String -Path $clientLog -Pattern "stream-inbound sample=Bingo .* name=.*Notify" -Quiet)) {
        throw "Bingo client did not write stream-inbound push marker."
    }

    $playLogs = @($playA, $playB)
    $sessionLogs = @($sessionA, $sessionB)
    Wait-LogCount -Path $playLogs -Pattern "bingo-record fetched actor=player-1 wins=0 losses=0" -Expected 1
    Wait-LogCount -Path $playLogs -Pattern "bingo-record fetched actor=player-2 wins=0 losses=0" -Expected 1
    Wait-LogCount -Path $playLogs -Pattern "bingo-record reported actor=player-1 wins=1 losses=0" -Expected 1
    Wait-LogCount -Path $playLogs -Pattern "bingo-record reported actor=player-2 wins=0 losses=1" -Expected 1
    Wait-LogCount -Path $playLogs -Pattern "bingo-lifecycle room-leave actor=player-1" -Expected 1
    Wait-LogCount -Path $playLogs -Pattern "bingo-lifecycle room-leave actor=player-2" -Expected 1
    Wait-LogCount -Path $playLogs -Pattern "bingo-lifecycle room-leave actor=observer" -Expected 1
    Wait-LogCount -Path $playLogs -Pattern "bingo-lifecycle entry-leave actor=player-1" -Expected 1
    Wait-LogCount -Path $playLogs -Pattern "bingo-lifecycle entry-leave actor=player-2" -Expected 1
    Wait-LogCount -Path $playLogs -Pattern "bingo-lifecycle entry-leave actor=observer" -Expected 1
    Wait-LogCount -Path $playLogs -Pattern "bingo-lifecycle entry-destroy-complete actor=player-1" -Expected 1
    Wait-LogCount -Path $playLogs -Pattern "bingo-lifecycle entry-destroy-complete actor=player-2" -Expected 1
    Wait-LogCount -Path $sessionLogs -Pattern "bingo-lifecycle session-disconnect actor=player-1 destroy=false" -Expected 1
    Wait-LogCount -Path $sessionLogs -Pattern "bingo-lifecycle session-disconnect actor=player-2 destroy=false" -Expected 1
    Wait-LogCount -Path $playLogs -Pattern "bingo-record reported actor=observer" -Expected 0
    Wait-LogCount -Path $playLogs -Pattern "bingo-lifecycle entry-destroy-complete actor=observer" -Expected 0
    Write-Host "bingo-placement=completed"
    $RunSucceeded = $true
}
finally {
    Remove-SampleConfigurationFiles -RunDirectory $RunDir
    Stop-SampleProcesses
    if ($RedisContainer) {
        Remove-SampleRedisContainer $RedisContainer
    }
    if (-not $RunSucceeded -or $env:BINGO_KEEP_RUN_DIR -eq "1") {
        Write-Host "runDir=$RunDir"
    }
    else {
        Remove-Item -Recurse -Force $RunDir -ErrorAction SilentlyContinue
    }
}

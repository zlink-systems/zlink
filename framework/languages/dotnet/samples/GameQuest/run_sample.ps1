$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptDir "../sample_runner.ps1")

$RunDir = New-SampleRunDirectory "gamequest-dotnet"
$RunId = "$PID-$([Guid]::NewGuid().ToString('N'))"
$RedisContainer = $null
$RunSucceeded = $false
$RunFinalized = $false
$GameQuestWaitAttempts = 300
$LogDir = Join-Path $RunDir "logs"
$SampleLogDir = Join-Path $RunDir "sample-logs"
New-Item -ItemType Directory -Force -Path $LogDir, $SampleLogDir | Out-Null

function Wait-GameQuestLogContains {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Pattern
    )

    for ($attempt = 0; $attempt -lt $GameQuestWaitAttempts; $attempt++) {
        if ((Test-Path -Path $Path -PathType Leaf) -and
            (Select-String -Path $Path -SimpleMatch $Pattern -Quiet)) {
            return
        }
        Start-Sleep -Milliseconds 100
    }

    throw "Timed out waiting for '$Pattern' in $Path"
}

function Get-GameQuestLogCount {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Pattern
    )

    if (-not (Test-Path -Path $Path -PathType Leaf)) { return 0 }
    return @((Select-String -Path $Path -SimpleMatch $Pattern)).Count
}

function Wait-GameQuestTotalAtLeast {
    param(
        [Parameter(Mandatory = $true)][int]$Expected,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string[]]$Paths
    )

    for ($attempt = 0; $attempt -lt $GameQuestWaitAttempts; $attempt++) {
        $total = @($Paths | ForEach-Object { Get-GameQuestLogCount $_ $Pattern } | Measure-Object -Sum).Sum
        if ($total -ge $Expected) { return }
        Start-Sleep -Milliseconds 100
    }

    throw "Timed out waiting for $Expected '$Pattern' row(s)"
}

try {
    $ports = New-SamplePorts -Count 10 -BasePort 0

    $GAMEQUEST_LOG_DIR = $SampleLogDir
    $GAMEQUEST_REDIS_KEY_PREFIX = "gamequest:dotnet:${RunId}:"
    $GAMEQUEST_GAMEAPI_A_HTTP_BASE_URL = "http://127.0.0.1:$($ports[0])"
    $GAMEQUEST_GAMEAPI_B_HTTP_BASE_URL = "http://127.0.0.1:$($ports[1])"
    $GAMEQUEST_API_A_STREAM_BIND_ENDPOINT = "tcp://127.0.0.1:$($ports[2])"
    $GAMEQUEST_API_B_STREAM_BIND_ENDPOINT = "tcp://127.0.0.1:$($ports[3])"
    $GAMEQUEST_GAMEAPI_A_STREAM_ENDPOINT = $GAMEQUEST_API_A_STREAM_BIND_ENDPOINT
    $GAMEQUEST_GAMEAPI_B_STREAM_ENDPOINT = $GAMEQUEST_API_B_STREAM_BIND_ENDPOINT
    $GAMEQUEST_MISSION_A_HTTP_URL = "http://127.0.0.1:$($ports[4])"
    $GAMEQUEST_MISSION_B_HTTP_URL = "http://127.0.0.1:$($ports[5])"
    $GAMEQUEST_GAMEAPI_A_MESH_ENDPOINT = "tcp://127.0.0.1:$($ports[6])"
    $GAMEQUEST_GAMEAPI_B_MESH_ENDPOINT = "tcp://127.0.0.1:$($ports[7])"
    $GAMEQUEST_MISSION_A_MESH_ENDPOINT = "tcp://127.0.0.1:$($ports[8])"
    $GAMEQUEST_MISSION_B_MESH_ENDPOINT = "tcp://127.0.0.1:$($ports[9])"
    $GAMEQUEST_CLOSE_REPLAY_RELEASE_FILE = Join-Path $RunDir "close-replay-release"
    $GAMEQUEST_OWNER_LOSS_RELEASE_FILE = Join-Path $RunDir "owner-loss-release"

    Invoke-SampleDotnetBuild (Join-Path $ScriptDir "GameQuest.csproj")

    $redis = Start-SampleRedisContainer "zlink-gamequest-dotnet-redis"
    $RedisContainer = $redis.ContainerId
    $GAMEQUEST_REDIS_ENDPOINT = $redis.Endpoint
    Wait-SampleTcpEndpoint "redis" "tcp://$GAMEQUEST_REDIS_ENDPOINT"
    $common = @{
        LogDirectory = $SampleLogDir
        RedisEndpoint = $GAMEQUEST_REDIS_ENDPOINT
        RedisKeyPrefix = $GAMEQUEST_REDIS_KEY_PREFIX
    }
    $configFiles = @{}
    $roles = @{
        "mission-a" = $common + @{
            InstanceName = "mission-a"; GameApiAHttpBaseUrl = $GAMEQUEST_GAMEAPI_A_HTTP_BASE_URL
            MissionAHttpBaseUrl = $GAMEQUEST_MISSION_A_HTTP_URL; MissionAMeshEndpoint = $GAMEQUEST_MISSION_A_MESH_ENDPOINT
        }
        "mission-b" = $common + @{
            InstanceName = "mission-b"; GameApiAHttpBaseUrl = $GAMEQUEST_GAMEAPI_A_HTTP_BASE_URL
            MissionBHttpBaseUrl = $GAMEQUEST_MISSION_B_HTTP_URL; MissionBMeshEndpoint = $GAMEQUEST_MISSION_B_MESH_ENDPOINT
        }
        "api-a" = $common + @{
            InstanceName = "api-a"; GameApiAHttpBaseUrl = $GAMEQUEST_GAMEAPI_A_HTTP_BASE_URL
            GameApiAStreamBindEndpoint = $GAMEQUEST_API_A_STREAM_BIND_ENDPOINT; GameApiAMeshEndpoint = $GAMEQUEST_GAMEAPI_A_MESH_ENDPOINT
        }
        "api-b" = $common + @{
            InstanceName = "api-b"; GameApiBHttpBaseUrl = $GAMEQUEST_GAMEAPI_B_HTTP_BASE_URL
            GameApiBStreamBindEndpoint = $GAMEQUEST_API_B_STREAM_BIND_ENDPOINT; GameApiBMeshEndpoint = $GAMEQUEST_GAMEAPI_B_MESH_ENDPOINT
        }
    }
    foreach ($instance in $roles.Keys) {
        $path = Join-Path $RunDir "appsettings.$instance.json"
        @{ Sample = $roles[$instance] } | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -Path $path
        $configFiles[$instance] = $path
    }
    $clientPath = Join-Path $RunDir "appsettings.client.json"
    @{ Client = @{
        GameApiAHttpBaseUrl = $GAMEQUEST_GAMEAPI_A_HTTP_BASE_URL; GameApiBHttpBaseUrl = $GAMEQUEST_GAMEAPI_B_HTTP_BASE_URL
        MissionAHttpBaseUrl = $GAMEQUEST_MISSION_A_HTTP_URL; MissionBHttpBaseUrl = $GAMEQUEST_MISSION_B_HTTP_URL
        GameApiAStreamEndpoint = $GAMEQUEST_GAMEAPI_A_STREAM_ENDPOINT; GameApiBStreamEndpoint = $GAMEQUEST_GAMEAPI_B_STREAM_ENDPOINT
        CloseReplayReleaseFile = $GAMEQUEST_CLOSE_REPLAY_RELEASE_FILE
        OwnerLossReleaseFile = $GAMEQUEST_OWNER_LOSS_RELEASE_FILE
    } } | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -Path $clientPath
    $configFiles["client"] = $clientPath

    $missionAProcess = Start-SampleDotnetAssembly -Name "mission-a" -Project (Join-Path $ScriptDir "Server/QuestMission/GameQuest.QuestMission.csproj") -LogDirectory $LogDir -Arguments @("--config", $configFiles["mission-a"])
    Wait-SampleTcpEndpoint "mission-a-mesh" $GAMEQUEST_MISSION_A_MESH_ENDPOINT -Attempts $GameQuestWaitAttempts
    Wait-SampleHttpHealth "mission-a" $GAMEQUEST_MISSION_A_HTTP_URL -Attempts $GameQuestWaitAttempts
    Wait-GameQuestLogContains (Join-Path $LogDir "mission-a.out.log") "gamequest-ready kind=instance-factory node=mission-a"

    $missionBProcess = Start-SampleDotnetAssembly -Name "mission-b" -Project (Join-Path $ScriptDir "Server/QuestMission/GameQuest.QuestMission.csproj") -LogDirectory $LogDir -Arguments @("--config", $configFiles["mission-b"])
    Wait-SampleTcpEndpoint "mission-b-mesh" $GAMEQUEST_MISSION_B_MESH_ENDPOINT -Attempts $GameQuestWaitAttempts
    Wait-SampleHttpHealth "mission-b" $GAMEQUEST_MISSION_B_HTTP_URL -Attempts $GameQuestWaitAttempts
    Wait-GameQuestLogContains (Join-Path $LogDir "mission-b.out.log") "gamequest-ready kind=instance-factory node=mission-b"

    Start-SampleDotnetAssembly -Name "api-a" -Project (Join-Path $ScriptDir "Server/GameApi/GameQuest.GameApi.csproj") -LogDirectory $LogDir -Arguments @("--config", $configFiles["api-a"]) | Out-Null
    Wait-SampleTcpEndpoint "api-a-stream" $GAMEQUEST_API_A_STREAM_BIND_ENDPOINT -Attempts $GameQuestWaitAttempts
    Wait-SampleTcpEndpoint "api-a-mesh" $GAMEQUEST_GAMEAPI_A_MESH_ENDPOINT -Attempts $GameQuestWaitAttempts
    Wait-SampleHttpHealth "api-a" $GAMEQUEST_GAMEAPI_A_HTTP_BASE_URL -Attempts $GameQuestWaitAttempts
    Wait-GameQuestLogContains (Join-Path $LogDir "api-a.out.log") "gamequest-ready kind=stream node=api-a"
    Wait-GameQuestLogContains (Join-Path $LogDir "api-a.out.log") "gamequest-ready kind=spot-route node=api-a mesh=gamequest"

    Start-SampleDotnetAssembly -Name "api-b" -Project (Join-Path $ScriptDir "Server/GameApi/GameQuest.GameApi.csproj") -LogDirectory $LogDir -Arguments @("--config", $configFiles["api-b"]) | Out-Null
    Wait-SampleTcpEndpoint "api-b-stream" $GAMEQUEST_API_B_STREAM_BIND_ENDPOINT -Attempts $GameQuestWaitAttempts
    Wait-SampleTcpEndpoint "api-b-mesh" $GAMEQUEST_GAMEAPI_B_MESH_ENDPOINT -Attempts $GameQuestWaitAttempts
    Wait-SampleHttpHealth "api-b" $GAMEQUEST_GAMEAPI_B_HTTP_BASE_URL -Attempts $GameQuestWaitAttempts
    Wait-GameQuestLogContains (Join-Path $LogDir "api-b.out.log") "gamequest-ready kind=stream node=api-b"
    Wait-GameQuestLogContains (Join-Path $LogDir "api-b.out.log") "gamequest-ready kind=spot-route node=api-b mesh=gamequest"

    $clientProject = Join-Path $ScriptDir "Client/GameQuest.Client.csproj"
    $clientAssembly = Join-Path (Split-Path -Parent $clientProject) "bin/Debug/net8.0/GameQuest.Client.dll"
    $clientLog = Join-Path $LogDir "client.out.log"
    $clientErrorLog = Join-Path $LogDir "client.err.log"
    $clientProcess = Start-Process -FilePath "dotnet" -ArgumentList @($clientAssembly, "--config", $configFiles["client"]) -RedirectStandardOutput $clientLog -RedirectStandardError $clientErrorLog -PassThru
    $script:SampleProcesses += $clientProcess
    Wait-GameQuestLogContains $clientLog "gamequest-client close-replay-armed player=player-alice"
    $closedMission = $null
    for ($attempt = 0; $attempt -lt $GameQuestWaitAttempts; $attempt++) {
        if ((Get-GameQuestLogCount (Join-Path $LogDir "mission-a.out.log") "gamequest-owner closed player=player-alice generation=1 node=mission-a") -gt 0) {
            $closedMission = "mission-a"
            break
        }
        if ((Get-GameQuestLogCount (Join-Path $LogDir "mission-b.out.log") "gamequest-owner closed player=player-alice generation=1 node=mission-b") -gt 0) {
            $closedMission = "mission-b"
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if ($null -eq $closedMission) { throw "Timed out waiting for the player-alice owner-closed marker" }
    New-Item -ItemType File -Path $GAMEQUEST_CLOSE_REPLAY_RELEASE_FILE | Out-Null
    Wait-GameQuestLogContains $clientLog "gamequest-client owner-loss-armed player=player-alice"

    $ownerMission = $null
    for ($attempt = 0; $attempt -lt $GameQuestWaitAttempts; $attempt++) {
        if ((Get-GameQuestLogCount (Join-Path $LogDir "mission-a.out.log") "gamequest-owner ready player=player-alice generation=2 node=mission-a") -gt 0) {
            $ownerMission = "mission-a"
            break
        }
        if ((Get-GameQuestLogCount (Join-Path $LogDir "mission-b.out.log") "gamequest-owner ready player=player-alice generation=2 node=mission-b") -gt 0) {
            $ownerMission = "mission-b"
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if ($null -eq $ownerMission) { throw "Timed out waiting for the player-alice owner-ready marker" }
    $ownerProcess = if ($ownerMission -eq "mission-a") { $missionAProcess } else { $missionBProcess }
    $ownerProcess.Kill($true)
    $ownerProcess.WaitForExit()
    New-Item -ItemType File -Path $GAMEQUEST_OWNER_LOSS_RELEASE_FILE | Out-Null
    $clientProcess.WaitForExit()
    if ($clientProcess.ExitCode -ne 0) { throw "GameQuest client failed with exit code $($clientProcess.ExitCode)" }

    Wait-GameQuestTotalAtLeast 4 "gamequest-api event-routed player=" @((Join-Path $LogDir "api-a.out.log"), (Join-Path $LogDir "api-b.out.log"))
    Wait-GameQuestTotalAtLeast 4 "gamequest-mission processed player=" @((Join-Path $LogDir "mission-a.out.log"), (Join-Path $LogDir "mission-b.out.log"))
    Wait-GameQuestTotalAtLeast 1 "gamequest-mission reconciled player=player-alice quest=" @((Join-Path $LogDir "mission-a.out.log"), (Join-Path $LogDir "mission-b.out.log"))
    if ((Get-GameQuestLogCount (Join-Path $LogDir "mission-a.out.log") "gamequest-mission reconciled player=player-alice quest=") + (Get-GameQuestLogCount (Join-Path $LogDir "mission-b.out.log") "gamequest-mission reconciled player=player-alice quest=") -ne 1) { throw "Expected one reconcile row for player-alice" }
    Wait-GameQuestTotalAtLeast 1 "gamequest-mission replayed player=player-alice generation=" @((Join-Path $LogDir "mission-a.out.log"), (Join-Path $LogDir "mission-b.out.log"))
    if ((Get-GameQuestLogCount (Join-Path $LogDir "mission-a.out.log") "gamequest-mission replayed player=player-alice generation=") + (Get-GameQuestLogCount (Join-Path $LogDir "mission-b.out.log") "gamequest-mission replayed player=player-alice generation=") -ne 1) { throw "Expected one replay row for player-alice" }
    Wait-GameQuestTotalAtLeast 1 "gamequest-owner unavailable player=player-alice" @((Join-Path $LogDir "api-a.out.log"), (Join-Path $LogDir "api-b.out.log"))
    if ((Get-GameQuestLogCount (Join-Path $LogDir "api-a.out.log") "gamequest-owner unavailable player=player-alice") + (Get-GameQuestLogCount (Join-Path $LogDir "api-b.out.log") "gamequest-owner unavailable player=player-alice") -ne 1) { throw "Expected one unavailable row for player-alice" }
    if ((Get-GameQuestLogCount (Join-Path $LogDir "mission-a.out.log") "gamequest-owner replacement-handler-invoked player=player-alice") + (Get-GameQuestLogCount (Join-Path $LogDir "mission-b.out.log") "gamequest-owner replacement-handler-invoked player=player-alice") -ne 0) { throw "Replacement handler ran for player-alice" }
    Wait-GameQuestLogContains $clientLog "gamequest=completed"
    Wait-GameQuestLogContains $clientLog "gamequest-server-evidence=completed"
    if ((Get-GameQuestLogCount $clientLog "gamequest=completed") -ne 1) { throw "Client completion marker is missing" }
    if ((Get-GameQuestLogCount $clientLog "gamequest-server-evidence=completed") -ne 1) { throw "Client server-evidence marker is missing" }

    $RunSucceeded = $true
    Remove-SampleConfigurationFiles -RunDirectory $RunDir
    Stop-SampleProcesses
    if ($RedisContainer) {
        Remove-SampleRedisContainer $RedisContainer
    }
    if ($env:GAMEQUEST_KEEP_RUN_DIR -eq "1") {
        Write-Host "runDir=$RunDir"
    }
    else {
        Remove-Item -Recurse -Force $RunDir -ErrorAction SilentlyContinue
    }
    $RunFinalized = $true
    Write-Host "gamequest-placement=completed"
}
finally {
    if (-not $RunFinalized) {
        Remove-SampleConfigurationFiles -RunDirectory $RunDir
        Stop-SampleProcesses
        if ($RedisContainer) {
            Remove-SampleRedisContainer $RedisContainer
        }
        if (-not $RunSucceeded -or $env:GAMEQUEST_KEEP_RUN_DIR -eq "1") {
            Write-Host "runDir=$RunDir"
        }
        else {
            Remove-Item -Recurse -Force $RunDir -ErrorAction SilentlyContinue
        }
    }
}

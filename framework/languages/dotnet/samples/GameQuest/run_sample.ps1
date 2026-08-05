$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptDir "../sample_runner.ps1")

$RunDir = New-SampleRunDirectory "gamequest-dotnet"
$RunId = "$PID-$([Guid]::NewGuid().ToString('N'))"
$RedisContainer = $null
$RunSucceeded = $false
$LogDir = Join-Path $RunDir "logs"
$SampleLogDir = Join-Path $RunDir "sample-logs"
New-Item -ItemType Directory -Force -Path $LogDir, $SampleLogDir | Out-Null

try {
    $ports = New-SamplePorts -Count 10 -BasePort 0

    $GAMEQUEST_LOG_DIR = $SampleLogDir
    $GAMEQUEST_REDIS_KEY_PREFIX = "gamequest:dotnet:${RunId}:"
    $GAMEQUEST_GAMEAPI_A_HTTP_BASE_URL = "http://127.0.0.1:$($ports[0])"
    $GAMEQUEST_GAMEAPI_B_HTTP_BASE_URL = "http://127.0.0.1:$($ports[1])"
    $GAMEQUEST_GAMEAPI_A_STREAM_ENDPOINT = "ws://127.0.0.1:$($ports[0])/quest/ws"
    $GAMEQUEST_GAMEAPI_B_STREAM_ENDPOINT = "ws://127.0.0.1:$($ports[1])/quest/ws"
    $GAMEQUEST_API_A_STREAM_BIND_ENDPOINT = "tcp://127.0.0.1:$($ports[2])"
    $GAMEQUEST_API_B_STREAM_BIND_ENDPOINT = "tcp://127.0.0.1:$($ports[3])"
    $GAMEQUEST_MISSION_A_HTTP_URL = "http://127.0.0.1:$($ports[4])"
    $GAMEQUEST_MISSION_B_HTTP_URL = "http://127.0.0.1:$($ports[5])"
    $GAMEQUEST_GAMEAPI_A_MESH_ENDPOINT = "tcp://127.0.0.1:$($ports[6])"
    $GAMEQUEST_GAMEAPI_B_MESH_ENDPOINT = "tcp://127.0.0.1:$($ports[7])"
    $GAMEQUEST_MISSION_A_MESH_ENDPOINT = "tcp://127.0.0.1:$($ports[8])"
    $GAMEQUEST_MISSION_B_MESH_ENDPOINT = "tcp://127.0.0.1:$($ports[9])"

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
    } } | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -Path $clientPath
    $configFiles["client"] = $clientPath

    Start-SampleDotnetAssembly -Name "mission-a" -Project (Join-Path $ScriptDir "Server/QuestMission/GameQuest.QuestMission.csproj") -LogDirectory $LogDir -Arguments @("--config", $configFiles["mission-a"]) | Out-Null
    Wait-SampleTcpEndpoint "mission-a-mesh" $GAMEQUEST_MISSION_A_MESH_ENDPOINT -Attempts 30
    Wait-SampleHttpHealth "mission-a" $GAMEQUEST_MISSION_A_HTTP_URL -Attempts 30

    Start-SampleDotnetAssembly -Name "mission-b" -Project (Join-Path $ScriptDir "Server/QuestMission/GameQuest.QuestMission.csproj") -LogDirectory $LogDir -Arguments @("--config", $configFiles["mission-b"]) | Out-Null
    Wait-SampleTcpEndpoint "mission-b-mesh" $GAMEQUEST_MISSION_B_MESH_ENDPOINT -Attempts 30
    Wait-SampleHttpHealth "mission-b" $GAMEQUEST_MISSION_B_HTTP_URL -Attempts 30

    Start-SampleDotnetAssembly -Name "api-a" -Project (Join-Path $ScriptDir "Server/GameApi/GameQuest.GameApi.csproj") -LogDirectory $LogDir -Arguments @("--config", $configFiles["api-a"]) | Out-Null
    Wait-SampleTcpEndpoint "api-a-stream" $GAMEQUEST_API_A_STREAM_BIND_ENDPOINT -Attempts 30
    Wait-SampleTcpEndpoint "api-a-mesh" $GAMEQUEST_GAMEAPI_A_MESH_ENDPOINT -Attempts 30
    Wait-SampleHttpHealth "api-a" $GAMEQUEST_GAMEAPI_A_HTTP_BASE_URL -Attempts 30

    Start-SampleDotnetAssembly -Name "api-b" -Project (Join-Path $ScriptDir "Server/GameApi/GameQuest.GameApi.csproj") -LogDirectory $LogDir -Arguments @("--config", $configFiles["api-b"]) | Out-Null
    Wait-SampleTcpEndpoint "api-b-stream" $GAMEQUEST_API_B_STREAM_BIND_ENDPOINT -Attempts 30
    Wait-SampleTcpEndpoint "api-b-mesh" $GAMEQUEST_GAMEAPI_B_MESH_ENDPOINT -Attempts 30
    Wait-SampleHttpHealth "api-b" $GAMEQUEST_GAMEAPI_B_HTTP_BASE_URL -Attempts 30

    Invoke-SampleDotnetRun -Project (Join-Path $ScriptDir "Client/GameQuest.Client.csproj") -Arguments @("--config", $configFiles["client"])

    Assert-SampleLogContains -LogDirectory $LogDir -Pattern "gamequest api event routed"
    Assert-SampleLogContains -LogDirectory $LogDir -Pattern "gamequest mission processed"
    Assert-SampleLogContains -LogDirectory $LogDir -Pattern "gamequest player quest spot ready"
    Invoke-WebRequest -Method Get -Uri "$($GAMEQUEST_MISSION_A_HTTP_URL)/self-check/events" -UseBasicParsing | Select-String -Pattern "QuestReconciled" | Out-Null
    Invoke-WebRequest -Method Post -Uri "$($GAMEQUEST_GAMEAPI_A_HTTP_BASE_URL)/self-check/assert" -UseBasicParsing | Select-String -Pattern '"passed":true' | Out-Null
    Write-Host "gamequest-server-evidence=completed"
    $RunSucceeded = $true
}
finally {
    Remove-SampleConfigurationFiles -RunDirectory $RunDir
    Stop-SampleProcesses
    if ($RedisContainer) {
        Remove-SampleRedisContainer $RedisContainer
    }
    if (-not $RunSucceeded -or $GAMEQUEST_KEEP_RUN_DIR -eq "1") {
        Write-Host "runDir=$RunDir"
    }
    else {
        Remove-Item -Recurse -Force $RunDir -ErrorAction SilentlyContinue
    }
}

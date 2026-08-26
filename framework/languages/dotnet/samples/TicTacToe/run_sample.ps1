$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptDir "../sample_runner.ps1")

$RunDir = New-SampleRunDirectory "tictactoe-dotnet"
$RunId = "$PID-$([Guid]::NewGuid().ToString('N'))"
$LogDir = Join-Path $RunDir "logs"
$SampleLogDir = Join-Path $RunDir "sample-logs"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
New-Item -ItemType Directory -Force -Path $SampleLogDir | Out-Null
$TICTACTOE_LOG_DIR = $SampleLogDir
$redisContainerId = $null
$RunSucceeded = $false

function Wait-LogCount {
    param(
        [Parameter(Mandatory = $true)][string[]]$Path,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][int]$Expected,
        [int]$Attempts = 300
    )

    for ($i = 0; $i -lt $Attempts; $i++) {
        $actual = @(Select-String -Path $Path -Pattern $Pattern -SimpleMatch -ErrorAction SilentlyContinue).Count
        if ($actual -eq $Expected) {
            return
        }
        Start-Sleep -Milliseconds 100
    }

    throw "Expected $Expected '$Pattern' entries in $($Path -join ', '), but the count did not converge."
}

try {
    $TICTACTOE_REDIS_KEY_PREFIX = "tictactoe:dotnet:${RunId}:"

    $ports = New-SamplePorts -Count 10 -BasePort 0

    $apiABindUrl = "http://127.0.0.1:$($ports[0])"
    $apiBBindUrl = "http://127.0.0.1:$($ports[1])"
    $apiAPublicUrl = $apiABindUrl
    $apiBPublicUrl = $apiBBindUrl
    $apiAMeshEndpoint = "tcp://127.0.0.1:$($ports[2])"
    $apiBMeshEndpoint = "tcp://127.0.0.1:$($ports[3])"
    $playAMeshEndpoint = "tcp://127.0.0.1:$($ports[4])"
    $playBMeshEndpoint = "tcp://127.0.0.1:$($ports[5])"
    $playAEndpoint = "tcp://127.0.0.1:$($ports[6])"
    $playBEndpoint = "tcp://127.0.0.1:$($ports[7])"
    $apiAChannelEndpoint = "tcp://127.0.0.1:$($ports[8])"
    $apiBChannelEndpoint = "tcp://127.0.0.1:$($ports[9])"
    $apiAConfigFile = Join-Path $RunDir "appsettings.api-a.json"
    $apiBConfigFile = Join-Path $RunDir "appsettings.api-b.json"
    $playAConfigFile = Join-Path $RunDir "appsettings.play-a.json"
    $playBConfigFile = Join-Path $RunDir "appsettings.play-b.json"
    $clientConfigFile = Join-Path $RunDir "appsettings.client.json"

    $redis = Start-SampleRedisContainer "zlink-tictactoe-dotnet-redis"
    $redisContainerId = $redis.ContainerId
    $TICTACTOE_REDIS_ENDPOINT = $redis.Endpoint
    $redisEndpoint = $TICTACTOE_REDIS_ENDPOINT

    function New-TicTacToeApiSettings {
        param(
            [string]$InstanceName,
            [string]$ApiBindUrl,
            [string]$MeshEndpoint,
            [string]$ApiChannelListenEndpoint
        )

        @{
            Sample = @{
                InstanceName = $InstanceName
                ApiBindUrl = $ApiBindUrl
                MeshEndpoint = $MeshEndpoint
                PeerMeshEndpoints = @($playAMeshEndpoint, $playBMeshEndpoint)
                ApiChannelListenEndpoint = $ApiChannelListenEndpoint
                PlayEndpoints = @($playAEndpoint, $playBEndpoint)
                RedisEndpoint = $redisEndpoint
                RedisKeyPrefix = $TICTACTOE_REDIS_KEY_PREFIX
                LogDirectory = $SampleLogDir
            }
        }
    }
    function New-TicTacToePlaySettings {
        param(
            [string]$InstanceName,
            [string]$MeshEndpoint,
            [string[]]$PeerMeshEndpoints,
            [string]$PlayEndpoint,
            [string[]]$ApiChannelPeerEndpoints
        )

        @{
            Sample = @{
                InstanceName = $InstanceName
                MeshEndpoint = $MeshEndpoint
                PeerMeshEndpoints = $PeerMeshEndpoints
                ApiChannelPeerEndpoints = $ApiChannelPeerEndpoints
                PlayEndpoint = $PlayEndpoint
                PlayEndpoints = @($playAEndpoint, $playBEndpoint)
                RedisEndpoint = $redisEndpoint
                RedisKeyPrefix = $TICTACTOE_REDIS_KEY_PREFIX
                LogDirectory = $SampleLogDir
            }
        }
    }
    New-TicTacToeApiSettings -InstanceName "api-a" -ApiBindUrl $apiABindUrl -MeshEndpoint $apiAMeshEndpoint -ApiChannelListenEndpoint $apiAChannelEndpoint | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -Path $apiAConfigFile
    New-TicTacToeApiSettings -InstanceName "api-b" -ApiBindUrl $apiBBindUrl -MeshEndpoint $apiBMeshEndpoint -ApiChannelListenEndpoint $apiBChannelEndpoint | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -Path $apiBConfigFile
    New-TicTacToePlaySettings -InstanceName "play-a" -MeshEndpoint $playAMeshEndpoint -PeerMeshEndpoints @() -PlayEndpoint $playAEndpoint -ApiChannelPeerEndpoints @($apiAChannelEndpoint, $apiBChannelEndpoint) | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -Path $playAConfigFile
    New-TicTacToePlaySettings -InstanceName "play-b" -MeshEndpoint $playBMeshEndpoint -PeerMeshEndpoints @($playAMeshEndpoint) -PlayEndpoint $playBEndpoint -ApiChannelPeerEndpoints @($apiAChannelEndpoint, $apiBChannelEndpoint) | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -Path $playBConfigFile
    @{ Sample = @{ ApiPublicUrls = @($apiAPublicUrl); LogDirectory = $LogDir } } | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -Path $clientConfigFile

    Invoke-SampleDotnetBuild (Join-Path $ScriptDir "TicTacToe.sln")

    Wait-SampleTcpEndpoint "redis" "tcp://$redisEndpoint"

    Start-SampleDotnetAssembly -Name "play-a" -Project (Join-Path $ScriptDir "Server/Play/TicTacToe.Server.Play.csproj") -LogDirectory $LogDir -Arguments @("--config", $playAConfigFile) | Out-Null
    Wait-SampleTcpEndpoint "play-a-stream" $playAEndpoint
    Wait-SampleTcpEndpoint "play-a-mesh" $playAMeshEndpoint

    Start-SampleDotnetAssembly -Name "play-b" -Project (Join-Path $ScriptDir "Server/Play/TicTacToe.Server.Play.csproj") -LogDirectory $LogDir -Arguments @("--config", $playBConfigFile) | Out-Null
    Wait-SampleTcpEndpoint "play-b-stream" $playBEndpoint
    Wait-SampleTcpEndpoint "play-b-mesh" $playBMeshEndpoint

    Start-SampleDotnetAssembly -Name "api-a" -Project (Join-Path $ScriptDir "Server/Api/TicTacToe.Server.Api.csproj") -LogDirectory $LogDir -Arguments @("--config", $apiAConfigFile) | Out-Null
    Wait-SampleTcpEndpoint "api-a-http" $apiABindUrl
    Wait-SampleTcpEndpoint "api-a-mesh" $apiAMeshEndpoint
    Wait-SampleTcpEndpoint "api-a-channel" $apiAChannelEndpoint

    Start-SampleDotnetAssembly -Name "api-b" -Project (Join-Path $ScriptDir "Server/Api/TicTacToe.Server.Api.csproj") -LogDirectory $LogDir -Arguments @("--config", $apiBConfigFile) | Out-Null
    Wait-SampleTcpEndpoint "api-b-http" $apiBBindUrl
    Wait-SampleTcpEndpoint "api-b-mesh" $apiBMeshEndpoint
    Wait-SampleTcpEndpoint "api-b-channel" $apiBChannelEndpoint

    Wait-LogCount (Join-Path $LogDir "play-a.log") "tictactoe-ready kind=peer-route node=play-a peer=play-b" 1
    Wait-LogCount (Join-Path $LogDir "play-b.log") "tictactoe-ready kind=peer-route node=play-b peer=play-a" 1
    Wait-LogCount (Join-Path $LogDir "api-a.log") "tictactoe-ready kind=http node=api-a" 1
    Wait-LogCount (Join-Path $LogDir "api-b.log") "tictactoe-ready kind=http node=api-b" 1
    Wait-LogCount (Join-Path $LogDir "api-a.log") "tictactoe-ready kind=spot-route node=api-a mesh=tictactoe" 1
    Wait-LogCount (Join-Path $LogDir "api-b.log") "tictactoe-ready kind=spot-route node=api-b mesh=tictactoe" 1

    $clientLog = Join-Path $LogDir "client.log"
    Invoke-SampleDotnetRun -Project (Join-Path $ScriptDir "Client/TicTacToe.Client.csproj") -Arguments @("--config", $clientConfigFile) *> $clientLog
    Wait-LogCount $clientLog "observer-connected endpoint=$playBEndpoint" 1
    Wait-LogCount $clientLog "observer-subscription=verified subscribed=true" 1
    Wait-LogCount $clientLog "observer-win-milestone=verified actor=player-x wins=100" 1
    Wait-LogCount $clientLog "reconnected-game-state=verified actor=player-x room=" 1
    Wait-LogCount $clientLog "tictactoe=completed" 1
    $playLogs = Join-Path $LogDir "play-*.log"
    Wait-LogCount $playLogs "tictactoe-lifecycle actor-bound actor=player-x" 1
    Wait-LogCount $playLogs "tictactoe-lifecycle leave-completed actor=player-x" 1
    Wait-LogCount $playLogs "tictactoe-lifecycle leave-completed actor=player-o" 1
    Wait-LogCount $playLogs "tictactoe-lifecycle actor-destroy-complete actor=player-x" 1
    Wait-LogCount $playLogs "tictactoe-lifecycle actor-destroy-complete actor=player-o" 1
    Wait-LogCount $playLogs "tictactoe-lifecycle actor-destroy-complete actor=observer" 0
    $dispatchError = Get-ChildItem -Path $LogDir -Filter "*.log" |
        Select-String -Pattern "dispatch-error" -List |
        Select-Object -First 1
    if ($null -ne $dispatchError) {
        throw "Unexpected dispatch-error in TicTacToe sample logs."
    }
    $RunSucceeded = $true
    Write-Host "tictactoe-placement=completed"
}
finally {
    Remove-SampleConfigurationFiles -RunDirectory $RunDir
    Stop-SampleProcesses
    if ($redisContainerId) {
        Remove-SampleRedisContainer $redisContainerId
    }
    if (-not $RunSucceeded -or $env:TICTACTOE_KEEP_RUN_DIR -eq "1") {
        Write-Host "runDir=$RunDir"
    }
    else {
        Remove-Item -Recurse -Force $RunDir -ErrorAction SilentlyContinue
    }
}

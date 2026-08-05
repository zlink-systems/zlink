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

function Wait-LogContains {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$Description,
        [int]$Attempts = 15
    )

    for ($i = 0; $i -lt $Attempts; $i++) {
        if (Select-String -Path $Path -Pattern $Pattern -Quiet) {
            return
        }
        Start-Sleep -Milliseconds 200
    }

    throw "$Description was not found."
}

try {
    $TICTACTOE_REDIS_KEY_PREFIX = "tictactoe:dotnet:${RunId}:"

    $ports = New-SamplePorts -Count 8 -BasePort 0

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
            [string]$MeshEndpoint
        )

        @{
            Sample = @{
                InstanceName = $InstanceName
                ApiBindUrl = $ApiBindUrl
                MeshEndpoint = $MeshEndpoint
                PeerMeshEndpoints = @($playAMeshEndpoint, $playBMeshEndpoint)
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
            [string]$PlayEndpoint
        )

        @{
            Sample = @{
                InstanceName = $InstanceName
                MeshEndpoint = $MeshEndpoint
                PeerMeshEndpoints = $PeerMeshEndpoints
                PlayEndpoint = $PlayEndpoint
                PlayEndpoints = @($playAEndpoint, $playBEndpoint)
                RedisEndpoint = $redisEndpoint
                RedisKeyPrefix = $TICTACTOE_REDIS_KEY_PREFIX
                LogDirectory = $SampleLogDir
            }
        }
    }
    New-TicTacToeApiSettings -InstanceName "api-a" -ApiBindUrl $apiABindUrl -MeshEndpoint $apiAMeshEndpoint | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -Path $apiAConfigFile
    New-TicTacToeApiSettings -InstanceName "api-b" -ApiBindUrl $apiBBindUrl -MeshEndpoint $apiBMeshEndpoint | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -Path $apiBConfigFile
    New-TicTacToePlaySettings -InstanceName "play-a" -MeshEndpoint $playAMeshEndpoint -PeerMeshEndpoints @() -PlayEndpoint $playAEndpoint | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -Path $playAConfigFile
    New-TicTacToePlaySettings -InstanceName "play-b" -MeshEndpoint $playBMeshEndpoint -PeerMeshEndpoints @($playAMeshEndpoint) -PlayEndpoint $playBEndpoint | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -Path $playBConfigFile
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

    Start-SampleDotnetAssembly -Name "api-b" -Project (Join-Path $ScriptDir "Server/Api/TicTacToe.Server.Api.csproj") -LogDirectory $LogDir -Arguments @("--config", $apiBConfigFile) | Out-Null
    Wait-SampleTcpEndpoint "api-b-http" $apiBBindUrl
    Wait-SampleTcpEndpoint "api-b-mesh" $apiBMeshEndpoint

    $clientLog = Join-Path $LogDir "client.log"
    Invoke-SampleDotnetRun -Project (Join-Path $ScriptDir "Client/TicTacToe.Client.csproj") -Arguments @("--config", $clientConfigFile) *> $clientLog
    Wait-LogContains $clientLog "stream-inbound sample=TicTacToe" "TicTacToe stream-inbound marker"
    Wait-LogContains $clientLog "stream-inbound sample=TicTacToe .* seq=[0-9]" "TicTacToe sequenced stream-inbound response marker"
    Wait-LogContains $clientLog "stream-inbound sample=TicTacToe .* name=.*Notify" "TicTacToe stream-inbound push marker"
    Wait-LogContains $clientLog "observer-win-milestone=verified" "TicTacToe observer win milestone notification"
    $playLogs = Join-Path $LogDir "play-*.log"
    Wait-LogContains $playLogs "actor: LeaveGameMsg completed. actor=player-x" "TicTacToe player-x LeaveGameMsg completion"
    Wait-LogContains $playLogs "actor: LeaveGameMsg completed. actor=player-o" "TicTacToe player-o LeaveGameMsg completion"
    Wait-LogContains $playLogs "entry spot: actor destroy completed. actor=player-x" "TicTacToe player-x destroy completion"
    Wait-LogContains $playLogs "entry spot: actor destroy completed. actor=player-o" "TicTacToe player-o destroy completion"
    $dispatchError = Get-ChildItem -Path $LogDir -Filter "*.log" |
        Select-String -Pattern "dispatch-error" -List |
        Select-Object -First 1
    if ($null -ne $dispatchError) {
        throw "Unexpected dispatch-error in TicTacToe sample logs."
    }
    $RunSucceeded = $true
}
finally {
    Remove-SampleConfigurationFiles -RunDirectory $RunDir
    Stop-SampleProcesses
    if ($redisContainerId) {
        Remove-SampleRedisContainer $redisContainerId
    }
    if (-not $RunSucceeded -or $TICTACTOE_KEEP_RUN_DIR -eq "1") {
        Write-Host "runDir=$RunDir"
    }
    else {
        Remove-Item -Recurse -Force $RunDir -ErrorAction SilentlyContinue
    }
}

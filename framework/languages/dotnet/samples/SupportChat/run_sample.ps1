$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptDir "../sample_runner.ps1")

$RunDir = New-SampleRunDirectory "supportchat-dotnet"
$RunId = "$PID-$([Guid]::NewGuid().ToString('N'))"
$RedisContainer = $null
$RunSucceeded = $false
$LogDir = Join-Path $RunDir "logs"
$SampleLogDir = Join-Path $RunDir "sample-logs"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
New-Item -ItemType Directory -Force -Path $SampleLogDir | Out-Null
$SUPPORTCHAT_LOG_DIR = $SampleLogDir
$SupportChatWaitAttempts = 300
$SupportChatWaitDelayMilliseconds = 100

function Get-SupportChatLogCount {
    param(
        [Parameter(Mandatory = $true)][string[]]$Path,
        [Parameter(Mandatory = $true)][string]$Pattern
    )

    $count = 0
    foreach ($entry in $Path) {
        if (Test-Path -LiteralPath $entry) {
            $count += @(Select-String -Path $entry -SimpleMatch -Pattern $Pattern).Count
        }
    }
    return $count
}

function Wait-SupportChatLogCount {
    param(
        [Parameter(Mandatory = $true)][string[]]$Path,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [int]$Expected = 1
    )

    for ($attempt = 1; $attempt -le $SupportChatWaitAttempts; $attempt++) {
        if ((Get-SupportChatLogCount -Path $Path -Pattern $Pattern) -ge $Expected) {
            return
        }
        Start-Sleep -Milliseconds $SupportChatWaitDelayMilliseconds
    }
    throw "Timed out waiting for at least $Expected occurrence(s) of '$Pattern'."
}

try {
    $ports = New-SamplePorts -Count 4 -BasePort 0

    $SUPPORTCHAT_SUPPORT_MESH_ENDPOINT = "tcp://127.0.0.1:$($ports[0])"
    $SUPPORTCHAT_API_MESH_ENDPOINT = "tcp://127.0.0.1:$($ports[1])"
    $SUPPORTCHAT_SESSION_MESH_ENDPOINT = "tcp://127.0.0.1:$($ports[2])"
    $SUPPORTCHAT_STREAM_ENDPOINT = "tcp://127.0.0.1:$($ports[3])"
    $SUPPORTCHAT_REDIS_KEY_PREFIX = "supportchat:dotnet:${RunId}:"

    $redis = Start-SampleRedisContainer "zlink-supportchat-dotnet-redis"
    $RedisContainer = $redis.ContainerId
    $SUPPORTCHAT_REDIS_ENDPOINT = $redis.Endpoint
    Wait-SampleTcpEndpoint "redis" "tcp://$SUPPORTCHAT_REDIS_ENDPOINT" -Attempts $SupportChatWaitAttempts
    $common = @{
        LogDirectory = $SampleLogDir
        RedisEndpoint = $SUPPORTCHAT_REDIS_ENDPOINT
        RedisKeyPrefix = $SUPPORTCHAT_REDIS_KEY_PREFIX
    }
    $supportConfigFile = Join-Path $RunDir "appsettings.support.json"
    $apiConfigFile = Join-Path $RunDir "appsettings.api.json"
    $sessionConfigFile = Join-Path $RunDir "appsettings.session.json"
    $clientConfigFile = Join-Path $RunDir "appsettings.client.json"
    @{ Sample = $common + @{
        MeshEndpoint = $SUPPORTCHAT_SUPPORT_MESH_ENDPOINT
    } } | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -Path $supportConfigFile
    @{ Sample = $common + @{
        MeshEndpoint = $SUPPORTCHAT_API_MESH_ENDPOINT
    } } | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -Path $apiConfigFile
    @{ Sample = $common + @{
        MeshEndpoint = $SUPPORTCHAT_SESSION_MESH_ENDPOINT
        StreamEndpoint = $SUPPORTCHAT_STREAM_ENDPOINT
    } } | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -Path $sessionConfigFile
    @{ Client = [ordered]@{
        LogDirectory = $SampleLogDir
        StreamEndpoint = $SUPPORTCHAT_STREAM_ENDPOINT
    } } | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -Path $clientConfigFile

    Invoke-SampleDotnetBuild (Join-Path $ScriptDir "SupportChat.csproj")

    Start-SampleDotnetAssembly -Name "support" -Project (Join-Path $ScriptDir "Server/Support/SupportChat.Server.Support.csproj") -LogDirectory $LogDir -Arguments @("--config", $supportConfigFile) | Out-Null
    Wait-SampleTcpEndpoint "support-mesh" $SUPPORTCHAT_SUPPORT_MESH_ENDPOINT -Attempts $SupportChatWaitAttempts
    Wait-SupportChatLogCount -Path (Join-Path $LogDir "support.out.log") -Pattern "supportchat-ready kind=public node=support"

    Start-SampleDotnetAssembly -Name "api" -Project (Join-Path $ScriptDir "Server/Api/SupportChat.Server.Api.csproj") -LogDirectory $LogDir -Arguments @("--config", $apiConfigFile) | Out-Null
    Wait-SampleTcpEndpoint "api-mesh" $SUPPORTCHAT_API_MESH_ENDPOINT -Attempts $SupportChatWaitAttempts
    Wait-SupportChatLogCount -Path (Join-Path $LogDir "api.out.log") -Pattern "supportchat-ready kind=public node=api"
    Wait-SupportChatLogCount -Path (Join-Path $LogDir "api.out.log") -Pattern "supportchat-ready kind=spot-route node=api mesh=supportchat"

    Start-SampleDotnetAssembly -Name "session" -Project (Join-Path $ScriptDir "Server/Session/SupportChat.Server.Session.csproj") -LogDirectory $LogDir -Arguments @("--config", $sessionConfigFile) | Out-Null
    Wait-SampleTcpEndpoint "session-mesh" $SUPPORTCHAT_SESSION_MESH_ENDPOINT -Attempts $SupportChatWaitAttempts
    Wait-SampleTcpEndpoint "session-stream" $SUPPORTCHAT_STREAM_ENDPOINT -Attempts $SupportChatWaitAttempts
    Wait-SupportChatLogCount -Path (Join-Path $LogDir "session.out.log") -Pattern "supportchat-ready kind=stream node=session"
    Wait-SupportChatLogCount -Path (Join-Path $LogDir "session.out.log") -Pattern "supportchat-ready kind=spot-route node=session mesh=supportchat"

    $clientLog = Join-Path $LogDir "client.log"
    Invoke-SampleDotnetRun -Project (Join-Path $ScriptDir "Client/SupportChat.Client.csproj") -Arguments @("--config", $clientConfigFile) *> $clientLog
    Wait-SupportChatLogCount -Path $clientLog -Pattern "supportchat=completed"
    Wait-SupportChatLogCount -Path $clientLog -Pattern "supportchat-closed-typing-ignore=verified"
    $apiAndSupportLogs = @(
        (Join-Path $LogDir "api.out.log"),
        (Join-Path $LogDir "support.out.log"))
    Wait-SupportChatLogCount -Path $apiAndSupportLogs -Pattern "supportchat-conversation created conversation="
    Wait-SupportChatLogCount -Path $apiAndSupportLogs -Pattern "supportchat-conversation agent-joined conversation="
    Wait-SupportChatLogCount -Path $apiAndSupportLogs -Pattern "supportchat-conversation status=WaitingForAgent conversation="
    Wait-SupportChatLogCount -Path $apiAndSupportLogs -Pattern "supportchat-conversation status=Active conversation="
    Wait-SupportChatLogCount -Path $apiAndSupportLogs -Pattern "supportchat-conversation status=WaitingForClose conversation="
    Wait-SupportChatLogCount -Path $apiAndSupportLogs -Pattern "supportchat-conversation status=Closed conversation="
    $RunSucceeded = $true
}
finally {
    Remove-SampleConfigurationFiles -RunDirectory $RunDir
    Stop-SampleProcesses
    if ($RedisContainer) {
        Remove-SampleRedisContainer $RedisContainer
    }
    if (-not $RunSucceeded -or $env:SUPPORTCHAT_KEEP_RUN_DIR -eq "1") {
        Write-Host "runDir=$RunDir"
    }
    else {
        Remove-Item -Recurse -Force $RunDir -ErrorAction SilentlyContinue
    }
}

Write-Host "supportchat-placement=completed"

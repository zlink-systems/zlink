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
    Wait-SampleTcpEndpoint "redis" "tcp://$SUPPORTCHAT_REDIS_ENDPOINT"
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
    Wait-SampleTcpEndpoint "support-mesh" $SUPPORTCHAT_SUPPORT_MESH_ENDPOINT

    Start-SampleDotnetAssembly -Name "api" -Project (Join-Path $ScriptDir "Server/Api/SupportChat.Server.Api.csproj") -LogDirectory $LogDir -Arguments @("--config", $apiConfigFile) | Out-Null
    Wait-SampleTcpEndpoint "api-mesh" $SUPPORTCHAT_API_MESH_ENDPOINT

    Start-SampleDotnetAssembly -Name "session" -Project (Join-Path $ScriptDir "Server/Session/SupportChat.Server.Session.csproj") -LogDirectory $LogDir -Arguments @("--config", $sessionConfigFile) | Out-Null
    Wait-SampleTcpEndpoint "session-mesh" $SUPPORTCHAT_SESSION_MESH_ENDPOINT
    Wait-SampleTcpEndpoint "session-stream" $SUPPORTCHAT_STREAM_ENDPOINT

    $clientLog = Join-Path $LogDir "client.log"
    Invoke-SampleDotnetRun -Project (Join-Path $ScriptDir "Client/SupportChat.Client.csproj") -Arguments @("--config", $clientConfigFile) *> $clientLog
    if (-not (Select-String -Path $clientLog -Pattern "supportchat=completed" -Quiet)) {
        throw "SupportChat client did not complete."
    }
    if (-not (Select-String -Path $clientLog -Pattern "supportchat-closed-typing-ignore=verified" -Quiet)) {
        throw "SupportChat client did not verify closed typing ignore."
    }

    Assert-SampleLogContains -LogDirectory $LogDir -Pattern "support conversation: created"
    Assert-SampleLogContains -LogDirectory $LogDir -Pattern "support conversation: actor joined"
    Assert-SampleLogContains -LogDirectory $LogDir -Pattern "status=WaitingForAgent"
    Assert-SampleLogContains -LogDirectory $LogDir -Pattern "status=Active"
    Assert-SampleLogContains -LogDirectory $LogDir -Pattern "status=WaitingForClose"
    Assert-SampleLogContains -LogDirectory $LogDir -Pattern "status=Closed"
    Write-Host "supportchat-server-evidence=completed"
    $RunSucceeded = $true
}
finally {
    Remove-SampleConfigurationFiles -RunDirectory $RunDir
    Stop-SampleProcesses
    if ($RedisContainer) {
        Remove-SampleRedisContainer $RedisContainer
    }
    if (-not $RunSucceeded -or $SUPPORTCHAT_KEEP_RUN_DIR -eq "1") {
        Write-Host "runDir=$RunDir"
    }
    else {
        Remove-Item -Recurse -Force $RunDir -ErrorAction SilentlyContinue
    }
}

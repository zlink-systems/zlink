$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. "$PSScriptRoot/../redis-common.ps1"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$CppRoot = Resolve-Path (Join-Path $ScriptDir "../..")
$BuildDir = if ($env:ZLINK_CPP_BUILD_DIR) { $env:ZLINK_CPP_BUILD_DIR } else { Join-Path $CppRoot "build" }
$WaitAttempts = 300
$WaitMilliseconds = 100
$Processes = New-Object System.Collections.Generic.List[System.Diagnostics.Process]
$RedisContainer = $null
$RunDir = Join-Path ([System.IO.Path]::GetTempPath()) "supportchat-cpp-$PID-$([Guid]::NewGuid().ToString('N'))"
$LogDir = Join-Path $RunDir "logs"
$FlowLogDir = Join-Path $RunDir "flow-logs"
$ConfigDir = Join-Path $RunDir "config"
New-Item -ItemType Directory -Force -Path $LogDir, $FlowLogDir, $ConfigDir | Out-Null

function Find-Binary([string]$Name) {
    foreach ($candidate in @(
        (Join-Path $BuildDir $Name), (Join-Path $BuildDir "$Name.exe"),
        (Join-Path $BuildDir "linux-ninja-debug/$Name"), (Join-Path $BuildDir "linux-ninja-debug/$Name.exe")
    )) { if (Test-Path $candidate) { return $candidate } }
    throw "Missing executable: $Name"
}
function Role-Logs([string]$Name) { return @((Join-Path $LogDir "$Name.stdout.log"), (Join-Path $LogDir "$Name.stderr.log")) }
function Get-ExactLineCount([string[]]$Paths, [string]$ExpectedLine) {
    $count = 0
    foreach ($path in $Paths) { if (Test-Path $path) { $count += @(Get-Content -Path $path | Where-Object { $_ -ceq $ExpectedLine }).Count } }
    return $count
}
function Get-PrefixCount([string[]]$Paths, [string]$Prefix) {
    $count = 0
    foreach ($path in $Paths) { if (Test-Path $path) { $count += @(Get-Content -Path $path | Where-Object { $_.StartsWith($Prefix, [System.StringComparison]::Ordinal) }).Count } }
    return $count
}
function Wait-ExactLineCount([string]$Name, [string[]]$Paths, [string]$ExpectedLine, [int]$ExpectedCount) {
    $actual = 0
    for ($attempt = 0; $attempt -lt $WaitAttempts; $attempt++) {
        $actual = Get-ExactLineCount $Paths $ExpectedLine
        if ($actual -eq $ExpectedCount) { return }
        if ($actual -gt $ExpectedCount) { break }
        Start-Sleep -Milliseconds $WaitMilliseconds
    }
    throw "Expected $Name exactly $ExpectedCount time(s), found $actual."
}
function Wait-PrefixMinimum([string]$Name, [string[]]$Paths, [string]$Prefix, [int]$Minimum) {
    $actual = 0
    for ($attempt = 0; $attempt -lt $WaitAttempts; $attempt++) {
        $actual = Get-PrefixCount $Paths $Prefix
        if ($actual -ge $Minimum) { return }
        Start-Sleep -Milliseconds $WaitMilliseconds
    }
    throw "Expected $Name at least $Minimum time(s), found $actual."
}
function Wait-Endpoint([string]$Name, [string]$Endpoint) {
    $value = $Endpoint -replace '^tcp://', '' -replace '^http://', ''
    $separator = $value.LastIndexOf(':')
    $hostName = $value.Substring(0, $separator)
    $port = [int]$value.Substring($separator + 1)
    for ($attempt = 0; $attempt -lt $WaitAttempts; $attempt++) {
        $client = [System.Net.Sockets.TcpClient]::new()
        try {
            $pending = $client.BeginConnect($hostName, $port, $null, $null)
            if ($pending.AsyncWaitHandle.WaitOne($WaitMilliseconds)) { $client.EndConnect($pending); return }
        } catch {} finally { $client.Close() }
        Start-Sleep -Milliseconds $WaitMilliseconds
    }
    throw "Timed out waiting for $Name at $Endpoint"
}
function Start-Role([string]$Name, [string]$Binary, [string[]]$Arguments) {
    $process = Start-Process -FilePath $Binary -ArgumentList $Arguments -PassThru `
        -RedirectStandardOutput (Join-Path $LogDir "$Name.stdout.log") `
        -RedirectStandardError (Join-Path $LogDir "$Name.stderr.log")
    $Processes.Add($process)
    return $process
}
function Remove-TrackedProcess([System.Diagnostics.Process]$Process) { [void]$Processes.Remove($Process) }
function Write-RoleConfig([string]$RoleName) {
    $configuration = @{ sample = @{ role = @{ name = $RoleName; logDir = $FlowLogDir }; topology = @{
        redisEndpoint = $RedisEndpoint; redisKeyPrefix = $RedisKeyPrefix; apiRouteEndpoint = $ApiRoute; apiSpotRouteEndpoint = $ApiSpotRoute
        supportRouteEndpoint = $SupportRoute; supportSpotRouterEndpoint = $SupportSpotRouter; supportSpotEndpoint = $SupportSpot
        supportHttpUrl = $SupportHttpUrl; supportActorRouteEndpoint = $SupportActorRoute; sessionStreamEndpoint = $SessionStream
        sessionSpotRouterEndpoint = $SessionSpotRouter; sessionSpotEndpoint = $SessionSpot; sessionActorRouteEndpoint = $SessionActorRoute
    } } }
    $configuration | ConvertTo-Json -Depth 6 | Set-Content -Path (Join-Path $ConfigDir "$RoleName.json") -Encoding utf8
}
function Cleanup {
    foreach ($process in @($Processes)) {
        try { if (-not $process.HasExited) { Stop-Process -Id $process.Id -ErrorAction SilentlyContinue }; [void]$process.WaitForExit(1000) } catch {}
    }
    if ($RedisContainer) { Remove-ZlinkSampleRedis $RedisContainer }
    if (Test-Path $RunDir) { Remove-Item -Recurse -Force $RunDir }
}

$Succeeded = $false
try {
    & cmake --build $BuildDir --parallel 2 --target sample_cpp_framework_supportchat_api sample_cpp_framework_supportchat_session sample_cpp_framework_supportchat_support sample_cpp_framework_supportchat_client
    if ($LASTEXITCODE -ne 0) { throw "SupportChat sample build failed." }
    $ports = @(Get-ZlinkSamplePorts -Count 12)
    $ApiRoute = "tcp://127.0.0.1:$($ports[1])"; $SupportRoute = "tcp://127.0.0.1:$($ports[2])"; $SupportSpotRouter = "tcp://127.0.0.1:$($ports[3])"
    $SupportSpot = "tcp://127.0.0.1:$($ports[4])"; $SessionStream = "tcp://127.0.0.1:$($ports[5])"; $SessionSpotRouter = "tcp://127.0.0.1:$($ports[6])"
    $SessionSpot = "tcp://127.0.0.1:$($ports[7])"; $SupportHttpUrl = "http://127.0.0.1:$($ports[8])"; $SessionActorRoute = "tcp://127.0.0.1:$($ports[9])"
    $SupportActorRoute = "tcp://127.0.0.1:$($ports[10])"; $ApiSpotRoute = "tcp://127.0.0.1:$($ports[11])"
    $redis = Start-ZlinkSampleRedis "zlink-redis-cpp-sample-supportchat" "redis:7-alpine"
    $RedisContainer = $redis.ContainerId; $RedisEndpoint = "tcp://$($redis.Endpoint)"; $RedisKeyPrefix = "supportchat:cpp:${PID}:$([Guid]::NewGuid().ToString('N')):"
    Wait-Endpoint "redis" $RedisEndpoint
    Write-RoleConfig "api"; Write-RoleConfig "session"; Write-RoleConfig "support"
    $api = Start-Role "api" (Find-Binary "sample_cpp_framework_supportchat_api") @("--config=$(Join-Path $ConfigDir 'api.json')")
    $session = Start-Role "session" (Find-Binary "sample_cpp_framework_supportchat_session") @("--config=$(Join-Path $ConfigDir 'session.json')")
    $support = Start-Role "support" (Find-Binary "sample_cpp_framework_supportchat_support") @("--config=$(Join-Path $ConfigDir 'support.json')")
    Wait-ExactLineCount "Api public readiness" (Role-Logs "api") "supportchat-ready kind=public node=api" 1
    Wait-ExactLineCount "Support public readiness" (Role-Logs "support") "supportchat-ready kind=public node=support" 1
    Wait-ExactLineCount "Session stream readiness" (Role-Logs "session") "supportchat-ready kind=stream node=session" 1
    Wait-ExactLineCount "Api Support spot route readiness" (Role-Logs "api") "supportchat-ready kind=spot-route node=api mesh=supportchat.support.spot" 1
    Wait-ExactLineCount "Session Support spot route readiness" (Role-Logs "session") "supportchat-ready kind=spot-route node=session mesh=supportchat.support.spot" 1
    $client = Start-Role "client" (Find-Binary "sample_cpp_framework_supportchat_client") @("--stream-endpoint", $SessionStream)
    Wait-ExactLineCount "client authentication" (Role-Logs "client") "supportchat authentication=verified" 1
    Wait-ExactLineCount "client conversation assignment" (Role-Logs "client") "supportchat conversation-assignment=verified" 1
    Wait-ExactLineCount "client bound push" (Role-Logs "client") "supportchat bound-push=verified" 1
    Wait-ExactLineCount "client reconnect" (Role-Logs "client") "supportchat reconnect=verified" 1
    Wait-ExactLineCount "client idle resume" (Role-Logs "client") "supportchat idle-resume=verified" 1
    Wait-ExactLineCount "client idle close" (Role-Logs "client") "supportchat idle-close=verified" 1
    Wait-ExactLineCount "client closed typing ignore" (Role-Logs "client") "supportchat-closed-typing-ignore=verified" 1
    Wait-ExactLineCount "client completion" (Role-Logs "client") "supportchat=completed" 1
    $serverLogs = @((Role-Logs "api") + (Role-Logs "support"))
    Wait-PrefixMinimum "conversation creation" $serverLogs "supportchat-conversation created conversation=" 1
    Wait-PrefixMinimum "agent conversation join" $serverLogs "supportchat-conversation agent-joined conversation=" 1
    Wait-PrefixMinimum "WaitingForAgent transition" $serverLogs "supportchat-conversation status=WaitingForAgent conversation=" 1
    Wait-PrefixMinimum "Active transition" $serverLogs "supportchat-conversation status=Active conversation=" 1
    Wait-PrefixMinimum "WaitingForClose transition" $serverLogs "supportchat-conversation status=WaitingForClose conversation=" 1
    Wait-PrefixMinimum "Closed transition" $serverLogs "supportchat-conversation status=Closed conversation=" 1
    $client.WaitForExit(); Remove-TrackedProcess $client
    if ($client.ExitCode -ne 0) { throw "SupportChat client failed with status $($client.ExitCode)." }
    $Succeeded = $true
} finally { Cleanup }

if (-not $Succeeded) { exit 1 }
Write-Host "supportchat-placement=completed"

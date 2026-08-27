Set-StrictMode -Version Latest
. "$PSScriptRoot/../../redis-common.ps1"
$ErrorActionPreference = "Stop"

$SampleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RunDir = Join-Path ([IO.Path]::GetTempPath()) ("zlink-supportchat-" + [Guid]::NewGuid().ToString("N"))
$LogDir = Join-Path $RunDir "logs"
$Processes = [System.Collections.Generic.List[System.Diagnostics.Process]]::new()
$RedisContainer = $null
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

function Protect-ConfigFile([string]$Path) {
    if ($IsWindows) {
        $identity = [Security.Principal.WindowsIdentity]::GetCurrent().Name
        & icacls $Path /inheritance:r /grant:r "${identity}:(R,W)" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Could not restrict config file ACL: $Path" }
    } else {
        & chmod 0600 $Path
        if ($LASTEXITCODE -ne 0) { throw "Could not restrict config file mode: $Path" }
    }
}

function Get-LogCount([string[]]$Paths, [string]$Evidence) {
    return @(Select-String -Path $Paths -Pattern $Evidence -SimpleMatch -ErrorAction SilentlyContinue).Count
}

function Wait-LogCount([string[]]$Paths, [string]$Evidence, [int]$Expected) {
    for ($attempt = 0; $attempt -lt 300; $attempt++) {
        $actual = Get-LogCount $Paths $Evidence
        if ($actual -eq $Expected) { return }
        if ($actual -gt $Expected) { throw "Expected $Expected '$Evidence', found $actual." }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for $Expected '$Evidence'."
}

function Wait-LogAtLeast([string[]]$Paths, [string]$Evidence, [int]$Minimum) {
    for ($attempt = 0; $attempt -lt 300; $attempt++) {
        if ((Get-LogCount $Paths $Evidence) -ge $Minimum) { return }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for at least $Minimum '$Evidence'."
}

function Get-AppBin([string]$Project, [string]$Name) {
    $bin = Join-Path $SampleDir "$Project/build/install/$Name/bin/$Name"
    if ($IsWindows) { return "$bin.bat" }
    return $bin
}

function Start-Role([string]$Role, [string]$Project, [string]$Name, [string]$Config) {
    $log = Join-Path $LogDir "$Role.log"
    $process = Start-Process -FilePath (Get-AppBin $Project $Name) -ArgumentList @("--config", $Config) `
        -WorkingDirectory $SampleDir -NoNewWindow -RedirectStandardOutput $log `
        -RedirectStandardError "$log.err" -PassThru
    $Processes.Add($process)
}

function Cleanup([int]$Status) {
    if ($Status -ne 0) {
        Get-ChildItem $LogDir -Filter "*.log" -ErrorAction SilentlyContinue | ForEach-Object {
            [Console]::Error.WriteLine("===== $($_.FullName) =====")
            Get-Content $_.FullName -Tail 200 -ErrorAction SilentlyContinue | ForEach-Object {
                [Console]::Error.WriteLine($_)
            }
        }
    }
    foreach ($process in $Processes) {
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
    }
    if ($RedisContainer) { Remove-ZlinkSampleRedis $RedisContainer }
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $RunDir
}

$Status = 1
try {
    $ports = @(Get-ZlinkSampleApplicationPorts -Language Java -Count 8)
    $apiChannelEndpoint = "tcp://127.0.0.1:$($ports[0])"
    $supportChannelEndpoint = "tcp://127.0.0.1:$($ports[1])"
    $sessionStreamEndpoint = "tcp://127.0.0.1:$($ports[2])"
    $sessionRouterEndpoint = "tcp://127.0.0.1:$($ports[3])"
    $supportRouterEndpoint = "tcp://127.0.0.1:$($ports[4])"
    $apiRouterEndpoint = "tcp://127.0.0.1:$($ports[5])"
    $apiHttpEndpoint = "http://127.0.0.1:$($ports[6])"
    $supportHttpEndpoint = "http://127.0.0.1:$($ports[7])"
    $redis = Start-ZlinkSampleRedis "zlink-redis-java-sample-supportchat" -Language Java
    $RedisContainer = $redis.ContainerId
    $redisPrefix = "zlink:supportchat:sample:$PID:$([Guid]::NewGuid().ToString('N'))"

    $apiConfig = Join-Path $RunDir "api.properties"
    @(
        "sample.redisEndpoint=$($redis.Endpoint)",
        "sample.redisKeyPrefix=$redisPrefix",
        "sample.logDirectory=$LogDir",
        "sample.apiChannelEndpoint=$apiChannelEndpoint",
        "sample.apiSpotRouterEndpoint=$apiRouterEndpoint",
        "sample.apiHttpEndpoint=$apiHttpEndpoint"
    ) | Set-Content -Path $apiConfig -Encoding utf8NoBOM
    $sessionConfig = Join-Path $RunDir "session.properties"
    @(
        "sample.redisEndpoint=$($redis.Endpoint)",
        "sample.redisKeyPrefix=$redisPrefix",
        "sample.logDirectory=$LogDir",
        "sample.sessionStreamEndpoint=$sessionStreamEndpoint",
        "sample.sessionSpotRouterEndpoint=$sessionRouterEndpoint",
        "sample.supportSpotRouterEndpoint=$supportRouterEndpoint"
    ) | Set-Content -Path $sessionConfig -Encoding utf8NoBOM
    $supportConfig = Join-Path $RunDir "support.properties"
    @(
        "sample.redisEndpoint=$($redis.Endpoint)",
        "sample.redisKeyPrefix=$redisPrefix",
        "sample.logDirectory=$LogDir",
        "sample.supportChannelEndpoint=$supportChannelEndpoint",
        "sample.supportSpotRouterEndpoint=$supportRouterEndpoint",
        "sample.sessionSpotRouterEndpoint=$sessionRouterEndpoint",
        "sample.supportHttpEndpoint=$supportHttpEndpoint"
    ) | Set-Content -Path $supportConfig -Encoding utf8NoBOM
    Protect-ConfigFile $apiConfig; Protect-ConfigFile $sessionConfig; Protect-ConfigFile $supportConfig

    $gradle = if ($IsWindows) { Join-Path $SampleDir "../../gradlew.bat" } else { Join-Path $SampleDir "../../gradlew" }
    Invoke-ZlinkSampleGradleBuild -GradleExecutable $gradle -Arguments @(
        "--settings-file", "standalone.settings.gradle.kts", "--no-daemon", "--no-parallel", "--max-workers=1",
        ":Server:Api:installDist", ":Server:Session:installDist", ":Server:Support:installDist", ":Client:installDist"
    )

    Start-Role "support" "Server/Support" "Support" $supportConfig
    Start-Role "api" "Server/Api" "Api" $apiConfig
    Start-Role "session" "Server/Session" "Session" $sessionConfig

    Wait-LogCount @((Join-Path $LogDir "api.log")) "supportchat-ready kind=public node=api" 1
    Wait-LogCount @((Join-Path $LogDir "support.log")) "supportchat-ready kind=public node=support" 1
    Wait-LogCount @((Join-Path $LogDir "session.log")) "supportchat-ready kind=stream node=session" 1
    Wait-LogCount @((Join-Path $LogDir "api.log")) "supportchat-ready kind=spot-route node=api mesh=supportchat-actors" 1
    Wait-LogCount @((Join-Path $LogDir "session.log")) "supportchat-ready kind=spot-route node=session mesh=supportchat-actors" 1

    $clientLog = Join-Path $LogDir "client.log"
    & (Get-AppBin "Client" "Client") --stream-endpoint $sessionStreamEndpoint *> $clientLog
    if ($LASTEXITCODE -ne 0) { throw "SupportChat client scenario failed." }

    Wait-LogCount @($clientLog) "supportchat=completed" 1
    Wait-LogCount @($clientLog) "supportchat-closed-typing-ignore=verified" 1
    $serverLogs = @((Join-Path $LogDir "api.log"), (Join-Path $LogDir "support.log"))
    foreach ($evidence in @(
        "supportchat-conversation created conversation=",
        "supportchat-conversation agent-joined conversation=",
        "supportchat-conversation status=WaitingForAgent conversation=",
        "supportchat-conversation status=Active conversation=",
        "supportchat-conversation status=WaitingForClose conversation=",
        "supportchat-conversation status=Closed conversation="
    )) {
        Wait-LogAtLeast $serverLogs $evidence 1
    }
    $Status = 0
} finally {
    if ($Status -ne 0) { Cleanup $Status }
}

Cleanup 0
Write-Output "supportchat-placement=completed"

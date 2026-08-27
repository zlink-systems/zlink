Set-StrictMode -Version Latest
. "$PSScriptRoot/../../redis-common.ps1"
$ErrorActionPreference = "Stop"

$SampleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RunDir = Join-Path ([IO.Path]::GetTempPath()) ("zlink-gamequest-" + [Guid]::NewGuid().ToString("N"))
$LogDir = Join-Path $RunDir "logs"
$Processes = [System.Collections.Generic.List[System.Diagnostics.Process]]::new()
$RoleProcesses = @{}
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

function Wait-Port([int]$Port) {
    for ($attempt = 0; $attempt -lt 300; $attempt++) {
        $client = [System.Net.Sockets.TcpClient]::new()
        try {
            $connect = $client.BeginConnect("127.0.0.1", $Port, $null, $null)
            if ($connect.AsyncWaitHandle.WaitOne(100)) { $client.EndConnect($connect); return }
        } catch {
        } finally { $client.Close() }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for port $Port"
}

function Wait-Http([string]$Endpoint) {
    for ($attempt = 0; $attempt -lt 300; $attempt++) {
        try {
            Invoke-WebRequest -UseBasicParsing -TimeoutSec 1 "$Endpoint/health" | Out-Null
            return
        } catch {
            Start-Sleep -Milliseconds 100
        }
    }
    throw "Timed out waiting for $Endpoint"
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
    $RoleProcesses[$Role] = $process
    return $process
}

function Assert-ClientMarker([string]$Path, [string]$Marker) {
    if ((Get-LogCount @($Path) $Marker) -lt 1) { throw "Client marker was not found: $Marker" }
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
    $endpoints = @(Get-ZlinkSampleApplicationEndpoints -Language Java -Count 10)
    $apiAStreamPort = [int](($endpoints[0] -split ':')[1])
    $apiBStreamPort = [int](($endpoints[1] -split ':')[1])
    $apiAHttpPort = [int](($endpoints[2] -split ':')[1])
    $apiBHttpPort = [int](($endpoints[3] -split ':')[1])
    $missionAChannelPort = [int](($endpoints[4] -split ':')[1])
    $missionBChannelPort = [int](($endpoints[5] -split ':')[1])
    $missionAHttpPort = [int](($endpoints[6] -split ':')[1])
    $missionBHttpPort = [int](($endpoints[7] -split ':')[1])
    $missionARouterPort = [int](($endpoints[8] -split ':')[1])
    $missionBRouterPort = [int](($endpoints[9] -split ':')[1])
    $apiAStream = "tcp://127.0.0.1:$apiAStreamPort"
    $apiBStream = "tcp://127.0.0.1:$apiBStreamPort"
    $apiAHttp = "http://127.0.0.1:$apiAHttpPort"
    $apiBHttp = "http://127.0.0.1:$apiBHttpPort"
    $missionAChannel = "tcp://127.0.0.1:$missionAChannelPort"
    $missionBChannel = "tcp://127.0.0.1:$missionBChannelPort"
    $missionAHttp = "http://127.0.0.1:$missionAHttpPort"
    $missionBHttp = "http://127.0.0.1:$missionBHttpPort"
    $missionARouter = "tcp://127.0.0.1:$missionARouterPort"
    $missionBRouter = "tcp://127.0.0.1:$missionBRouterPort"
    $redis = Start-ZlinkSampleRedis "zlink-redis-java-sample-gamequest" -Language Java
    $RedisContainer = $redis.ContainerId
    $redisPrefix = "gamequest:java:${PID}:$([Guid]::NewGuid().ToString('N')):"

    function Write-RoleConfig([string]$Name, [string]$Instance, [string]$EndpointKey, [string]$Endpoint, [string]$HttpEndpoint, [string]$Router) {
        $path = Join-Path $RunDir "$Name.properties"
        @(
            "sample.instanceName=$Instance",
            "sample.logDirectory=$LogDir",
            "sample.$EndpointKey=$Endpoint",
            "sample.httpEndpoint=$HttpEndpoint",
            "sample.redisEndpoint=$($redis.Endpoint)",
            "sample.redisKeyPrefix=$redisPrefix",
            "sample.spotRouterEndpoint=$Router"
        ) | Set-Content -Path $path -Encoding utf8NoBOM
        Protect-ConfigFile $path
        return $path
    }
    function Write-ClientConfig([string]$Name, [string]$Scenario, [string]$ReleaseFile = "") {
        $path = Join-Path $RunDir "$Name.properties"
        @(
            "sample.apiAStreamEndpoint=$apiAStream",
            "sample.apiBStreamEndpoint=$apiBStream",
            "sample.apiAHttpEndpoint=$apiAHttp",
            "sample.apiBHttpEndpoint=$apiBHttp",
            "sample.scenario=$Scenario"
        ) + $(if ($ReleaseFile) { "sample.ownerUnavailableReleaseFile=$ReleaseFile" }) |
            Set-Content -Path $path -Encoding utf8NoBOM
        Protect-ConfigFile $path
        return $path
    }

    $missionAConfig = Write-RoleConfig "mission-a" "mission-a" "channelEndpoint" $missionAChannel $missionAHttp $missionARouter
    $missionBConfig = Write-RoleConfig "mission-b" "mission-b" "channelEndpoint" $missionBChannel $missionBHttp $missionBRouter
    $apiAConfig = Write-RoleConfig "api-a" "api-a" "streamEndpoint" $apiAStream $apiAHttp $missionAChannel
    $apiBConfig = Write-RoleConfig "api-b" "api-b" "streamEndpoint" $apiBStream $apiBHttp $missionBChannel
    $clientConfig = Write-ClientConfig "client" "full"
    $rehydrateConfig = Write-ClientConfig "rehydrate-client" "rehydrate"
    $releaseFile = Join-Path $RunDir "owner-unavailable.release"
    $ownerUnavailableConfig = Write-ClientConfig "owner-unavailable-client" "owner-unavailable" $releaseFile

    $gradle = if ($IsWindows) { Join-Path $SampleDir "../../gradlew.bat" } else { Join-Path $SampleDir "../../gradlew" }
    Push-Location "$SampleDir/../../.."
    try {
        Invoke-ZlinkSampleGradleBuild -GradleExecutable $gradle -Arguments @("--no-daemon", ":zlink-framework-core:jar", ":zlink-framework-spring-boot-starter:jar", ":zlink-framework-locations-redis:jar", ":zlink-stream-connector:jar", "--quiet")
    } finally { Pop-Location }
    Invoke-ZlinkSampleGradleBuild -GradleExecutable $gradle -Arguments @("--settings-file", "standalone.settings.gradle.kts", "--no-daemon", ":Server:GameApi:installDist", ":Server:QuestMission:installDist", ":Client:installDist", "--quiet")

    Start-Role "mission-a" "Server/QuestMission" "QuestMission" $missionAConfig | Out-Null
    Start-Role "mission-b" "Server/QuestMission" "QuestMission" $missionBConfig | Out-Null
    Wait-Port $missionARouterPort; Wait-Port $missionBRouterPort; Wait-Http $missionAHttp; Wait-Http $missionBHttp
    Start-Role "api-a" "Server/GameApi" "GameApi" $apiAConfig | Out-Null
    Start-Role "api-b" "Server/GameApi" "GameApi" $apiBConfig | Out-Null
    Wait-Port $apiAStreamPort; Wait-Port $apiBStreamPort; Wait-Port $missionAChannelPort; Wait-Port $missionBChannelPort
    Wait-Http $apiAHttp; Wait-Http $apiBHttp

    Wait-LogCount @((Join-Path $LogDir "mission-a.log")) "gamequest-ready kind=instance-factory node=mission-a" 1
    Wait-LogCount @((Join-Path $LogDir "mission-b.log")) "gamequest-ready kind=instance-factory node=mission-b" 1
    Wait-LogCount @((Join-Path $LogDir "api-a.log")) "gamequest-ready kind=stream node=api-a" 1
    Wait-LogCount @((Join-Path $LogDir "api-b.log")) "gamequest-ready kind=stream node=api-b" 1
    Wait-LogCount @((Join-Path $LogDir "api-a.log")) "gamequest-ready kind=spot-route node=api-a mesh=gamequest.player-quests" 1
    Wait-LogCount @((Join-Path $LogDir "api-b.log")) "gamequest-ready kind=spot-route node=api-b mesh=gamequest.player-quests" 1

    $clientLog = Join-Path $LogDir "client.log"
    & (Get-AppBin "Client" "Client") --config $clientConfig *> $clientLog
    if ($LASTEXITCODE -ne 0) { throw "Full client scenario failed." }
    Assert-ClientMarker $clientLog "gamequest=completed"
    Assert-ClientMarker $clientLog "gamequest-server-evidence=completed"
    $apiLogs = @((Join-Path $LogDir "api-a.log"), (Join-Path $LogDir "api-b.log"))
    $missionLogs = @((Join-Path $LogDir "mission-a.log"), (Join-Path $LogDir "mission-b.log"))
    Wait-LogAtLeast $apiLogs "gamequest-api event-routed player=" 4
    Wait-LogAtLeast $missionLogs "gamequest-mission processed player=" 4
    Wait-LogCount $missionLogs "gamequest-mission reconciled player=player-alice quest=first-hunt" 1

    $close = Invoke-RestMethod -Method Post "$missionAHttp/self-check/owner/player-alice/close"
    if (-not $close.closed) { throw "Owner close did not complete." }
    $rehydrateLog = Join-Path $LogDir "rehydrate-client.log"
    & (Get-AppBin "Client" "Client") --config $rehydrateConfig *> $rehydrateLog
    if ($LASTEXITCODE -ne 0) { throw "Rehydrate client scenario failed." }
    Wait-LogCount $missionLogs "gamequest-mission replayed player=player-alice generation=" 1

    $ownerClient = Start-Role "owner-unavailable-client" "Client" "Client" $ownerUnavailableConfig
    Wait-LogCount @((Join-Path $LogDir "mission-a.log"), (Join-Path $LogDir "mission-b.log")) "gamequest-owner-ready player=player-owner-unavailable" 1
    $ownerNode = if ((Get-LogCount @((Join-Path $LogDir "mission-a.log")) "gamequest-owner-ready player=player-owner-unavailable node=mission-a") -eq 1) { "mission-a" } else { "mission-b" }
    Stop-Process -Id $RoleProcesses[$ownerNode].Id -Force
    $ownerClientRelease = New-Item -ItemType File -Path $releaseFile -Force
    $ownerClient.WaitForExit()
    if ($ownerClient.ExitCode -ne 0) { throw "Owner unavailable client scenario failed." }
    Wait-LogCount @((Join-Path $LogDir "api-a.log")) "gamequest-owner unavailable player=player-owner-unavailable" 1
    Wait-LogCount $missionLogs "gamequest-owner replacement-handler-invoked player=player-owner-unavailable" 0

    $Status = 0
    Write-Output "gamequest-placement=completed"
} finally {
    Cleanup $Status
}

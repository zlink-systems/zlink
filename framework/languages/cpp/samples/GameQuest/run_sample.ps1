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
$RunDir = Join-Path ([System.IO.Path]::GetTempPath()) "gamequest-cpp-$PID-$([Guid]::NewGuid().ToString('N'))"
$LogDir = Join-Path $RunDir "logs"
$ConfigDir = Join-Path $RunDir "config"
$ReleaseFile = Join-Path $RunDir "owner-loss-release"
New-Item -ItemType Directory -Force -Path $LogDir, $ConfigDir | Out-Null

function Find-Binary([string]$Name) {
    foreach ($candidate in @(
        (Join-Path $BuildDir $Name), (Join-Path $BuildDir "$Name.exe"),
        (Join-Path $BuildDir "linux-ninja-debug/$Name"),
        (Join-Path $BuildDir "linux-ninja-debug/$Name.exe")
    )) {
        if (Test-Path $candidate) { return $candidate }
    }
    throw "Missing executable: $Name"
}

function Role-Logs([string]$Name) {
    return @((Join-Path $LogDir "$Name.stdout.log"), (Join-Path $LogDir "$Name.stderr.log"))
}

function Get-ExactLineCount([string[]]$Paths, [string]$ExpectedLine) {
    $count = 0
    foreach ($path in $Paths) {
        if (Test-Path $path) {
            $count += @(Get-Content -Path $path | Where-Object { $_ -ceq $ExpectedLine }).Count
        }
    }
    return $count
}

function Get-PrefixCount([string[]]$Paths, [string]$Prefix) {
    $count = 0
    foreach ($path in $Paths) {
        if (Test-Path $path) {
            $count += @(Get-Content -Path $path | Where-Object {
                $_.StartsWith($Prefix, [System.StringComparison]::Ordinal)
            }).Count
        }
    }
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

function Wait-PrefixExactCount([string]$Name, [string[]]$Paths, [string]$Prefix, [int]$ExpectedCount) {
    $actual = 0
    for ($attempt = 0; $attempt -lt $WaitAttempts; $attempt++) {
        $actual = Get-PrefixCount $Paths $Prefix
        if ($actual -eq $ExpectedCount) { return }
        if ($actual -gt $ExpectedCount) { break }
        Start-Sleep -Milliseconds $WaitMilliseconds
    }
    throw "Expected $Name exactly $ExpectedCount time(s), found $actual."
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
            if ($pending.AsyncWaitHandle.WaitOne($WaitMilliseconds)) {
                $client.EndConnect($pending)
                return
            }
        } catch {
        } finally {
            $client.Close()
        }
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

function Write-RoleConfig([string]$RoleName, [string]$ApiName, [string]$MissionName) {
    $configuration = @{
        sample = @{
            role = @{ name = $RoleName; logDir = $LogDir }
            topology = @{
                redisEndpoint = $RedisEndpoint; redisKeyPrefix = $RedisKeyPrefix
                apiAStreamEndpoint = $ApiAStreamEndpoint; apiBStreamEndpoint = $ApiBStreamEndpoint
                apiAHttpUrl = $ApiAHttpUrl; apiBHttpUrl = $ApiBHttpUrl
                missionARouteEndpoint = $MissionARouteEndpoint; missionBRouteEndpoint = $MissionBRouteEndpoint
                missionASpotRouteEndpoint = $MissionASpotRouteEndpoint; missionBSpotRouteEndpoint = $MissionBSpotRouteEndpoint
                missionASpotRouterEndpoint = $MissionASpotRouterEndpoint; missionBSpotRouterEndpoint = $MissionBSpotRouterEndpoint
                missionASpotEndpoint = $MissionASpotEndpoint; missionBSpotEndpoint = $MissionBSpotEndpoint
                apiASpotRouterEndpoint = $ApiASpotRouterEndpoint; apiBSpotRouterEndpoint = $ApiBSpotRouterEndpoint
                apiASpotRouteEndpoint = $ApiASpotRouteEndpoint; apiBSpotRouteEndpoint = $ApiBSpotRouteEndpoint
                apiName = $ApiName; missionName = $MissionName
            }
        }
    }
    $configuration | ConvertTo-Json -Depth 6 | Set-Content -Path (Join-Path $ConfigDir "$RoleName.json") -Encoding utf8
}

function Remove-TrackedProcess([System.Diagnostics.Process]$Process) {
    [void]$Processes.Remove($Process)
}

function Cleanup {
    foreach ($process in @($Processes)) {
        try {
            if (-not $process.HasExited) { Stop-Process -Id $process.Id -ErrorAction SilentlyContinue }
            [void]$process.WaitForExit(1000)
        } catch {
        }
    }
    if ($RedisContainer) { Remove-ZlinkSampleRedis $RedisContainer }
    if (Test-Path $RunDir) { Remove-Item -Recurse -Force $RunDir }
}

$Succeeded = $false
try {
    & cmake --build $BuildDir --parallel 2 --target `
        sample_cpp_framework_gamequest_game_api `
        sample_cpp_framework_gamequest_quest_mission `
        sample_cpp_framework_gamequest_client
    if ($LASTEXITCODE -ne 0) { throw "GameQuest sample build failed." }

    $ports = @(Get-ZlinkSamplePorts -Count 17)
    $ApiAStreamEndpoint = "tcp://127.0.0.1:$($ports[0])"
    $ApiBStreamEndpoint = "tcp://127.0.0.1:$($ports[1])"
    $ApiAHttpUrl = "http://127.0.0.1:$($ports[2])"
    $ApiBHttpUrl = "http://127.0.0.1:$($ports[3])"
    $MissionARouteEndpoint = "tcp://127.0.0.1:$($ports[4])"
    $MissionBRouteEndpoint = "tcp://127.0.0.1:$($ports[5])"
    $MissionASpotRouteEndpoint = "tcp://127.0.0.1:$($ports[6])"
    $MissionBSpotRouteEndpoint = "tcp://127.0.0.1:$($ports[7])"
    $MissionASpotRouterEndpoint = "tcp://127.0.0.1:$($ports[8])"
    $MissionBSpotRouterEndpoint = "tcp://127.0.0.1:$($ports[9])"
    $MissionASpotEndpoint = "tcp://127.0.0.1:$($ports[10])"
    $MissionBSpotEndpoint = "tcp://127.0.0.1:$($ports[11])"
    $ApiASpotRouterEndpoint = "tcp://127.0.0.1:$($ports[12])"
    $ApiBSpotRouterEndpoint = "tcp://127.0.0.1:$($ports[13])"
    $ApiASpotRouteEndpoint = "tcp://127.0.0.1:$($ports[14])"
    $ApiBSpotRouteEndpoint = "tcp://127.0.0.1:$($ports[15])"

    $redis = Start-ZlinkSampleRedis "zlink-redis-cpp-sample-gamequest" "redis:7-alpine"
    $RedisContainer = $redis.ContainerId
    $RedisEndpoint = "tcp://$($redis.Endpoint)"
    $RedisKeyPrefix = "gamequest:cpp:$($PID):$([Guid]::NewGuid().ToString('N')):"
    Wait-Endpoint "redis" $RedisEndpoint

    Write-RoleConfig "mission-a" "api-a" "mission-a"
    Write-RoleConfig "mission-b" "api-a" "mission-b"
    Write-RoleConfig "api-a" "api-a" "mission-a"
    Write-RoleConfig "api-b" "api-b" "mission-a"

    $MissionBin = Find-Binary "sample_cpp_framework_gamequest_quest_mission"
    $ApiBin = Find-Binary "sample_cpp_framework_gamequest_game_api"
    $ClientBin = Find-Binary "sample_cpp_framework_gamequest_client"
    $missionA = Start-Role "mission-a" $MissionBin @("--config=$(Join-Path $ConfigDir 'mission-a.json')")
    $missionB = Start-Role "mission-b" $MissionBin @("--config=$(Join-Path $ConfigDir 'mission-b.json')")
    Wait-ExactLineCount "mission-a instance factory readiness" (Role-Logs "mission-a") "gamequest-ready kind=instance-factory node=mission-a" 1
    Wait-ExactLineCount "mission-b instance factory readiness" (Role-Logs "mission-b") "gamequest-ready kind=instance-factory node=mission-b" 1

    [void](Start-Role "api-a" $ApiBin @("--config=$(Join-Path $ConfigDir 'api-a.json')"))
    [void](Start-Role "api-b" $ApiBin @("--config=$(Join-Path $ConfigDir 'api-b.json')"))
    Wait-ExactLineCount "api-a stream readiness" (Role-Logs "api-a") "gamequest-ready kind=stream node=api-a" 1
    Wait-ExactLineCount "api-b stream readiness" (Role-Logs "api-b") "gamequest-ready kind=stream node=api-b" 1
    Wait-ExactLineCount "api-a Mission spot route readiness" (Role-Logs "api-a") "gamequest-ready kind=spot-route node=api-a mesh=gamequest" 1
    Wait-ExactLineCount "api-b Mission spot route readiness" (Role-Logs "api-b") "gamequest-ready kind=spot-route node=api-b mesh=gamequest" 1

    $client = Start-Role "client" $ClientBin @(
        "--api-a-stream-endpoint", $ApiAStreamEndpoint, "--api-b-stream-endpoint", $ApiBStreamEndpoint,
        "--api-a-http-url", $ApiAHttpUrl, "--api-b-http-url", $ApiBHttpUrl,
        "--owner-loss-release-file", $ReleaseFile)
    Wait-ExactLineCount "owner-loss client stage" (Role-Logs "client") "gamequest-owner-loss-stage-ready player=player-owner-failure" 1

    $ownerNode = $null
    for ($attempt = 0; $attempt -lt $WaitAttempts; $attempt++) {
        if ((Get-ExactLineCount (Role-Logs "mission-a") "gamequest-owner ready player=player-owner-failure node=mission-a") -eq 1) {
            $ownerNode = "mission-a"; break
        }
        if ((Get-ExactLineCount (Role-Logs "mission-b") "gamequest-owner ready player=player-owner-failure node=mission-b") -eq 1) {
            $ownerNode = "mission-b"; break
        }
        Start-Sleep -Milliseconds $WaitMilliseconds
    }
    if (-not $ownerNode) { throw "owner-ready marker was not found for player-owner-failure" }
    $ownerProcess = if ($ownerNode -eq "mission-a") { $missionA } else { $missionB }
    Stop-Process -Id $ownerProcess.Id -Force
    $ownerProcess.WaitForExit()
    Remove-TrackedProcess $ownerProcess
    New-Item -ItemType File -Path $ReleaseFile | Out-Null

    $client.WaitForExit()
    Remove-TrackedProcess $client
    if ($client.ExitCode -ne 0) { throw "GameQuest client failed with status $($client.ExitCode)." }

    Wait-ExactLineCount "client self-check completion marker" (Role-Logs "client") "gamequest=completed" 1
    Wait-ExactLineCount "client server-evidence completion marker" (Role-Logs "client") "gamequest-server-evidence=completed" 1
    Wait-PrefixMinimum "Api event routing across nodes" @((Role-Logs "api-a") + (Role-Logs "api-b")) "gamequest-api event-routed player=" 4
    Wait-PrefixMinimum "Mission event processing across nodes" @((Role-Logs "mission-a") + (Role-Logs "mission-b")) "gamequest-mission processed player=" 4
    Wait-ExactLineCount "player-alice reconcile" @((Role-Logs "mission-a") + (Role-Logs "mission-b")) "gamequest-mission reconciled player=player-alice quest=first-hunt" 1
    Wait-PrefixExactCount "player-alice replay" @((Role-Logs "mission-a") + (Role-Logs "mission-b")) "gamequest-mission replayed player=player-alice generation=" 1
    Wait-ExactLineCount "owner unavailable" @((Role-Logs "api-a") + (Role-Logs "api-b")) "gamequest-owner unavailable player=player-owner-failure" 1
    Wait-ExactLineCount "replacement handler absence" @((Role-Logs "mission-a") + (Role-Logs "mission-b")) "gamequest-owner replacement-handler-invoked player=player-owner-failure" 0
    $Succeeded = $true
} finally {
    Cleanup
}

if (-not $Succeeded) { exit 1 }
Write-Host "gamequest-placement=completed"

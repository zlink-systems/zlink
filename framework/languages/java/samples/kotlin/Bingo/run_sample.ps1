Set-StrictMode -Version Latest
. "$PSScriptRoot/../../redis-common.ps1"
$ErrorActionPreference = "Stop"

$SampleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $SampleDir

$LogDir = Join-Path $SampleDir "build/sample-logs"
$ConfigDir = Join-Path ([IO.Path]::GetTempPath()) ("zlink-bingo-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $LogDir, $ConfigDir | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $LogDir "*.log")
$env:BINGO_LOG_DIR = if ($env:BINGO_LOG_DIR) { $env:BINGO_LOG_DIR } else { Join-Path $SampleDir "logs" }
$env:ZLINK_JAVA_STREAM_TRACE = if ($env:ZLINK_JAVA_STREAM_TRACE) { $env:ZLINK_JAVA_STREAM_TRACE } else { "1" }
New-Item -ItemType Directory -Force -Path $env:BINGO_LOG_DIR | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $env:BINGO_LOG_DIR "*.log")

$Gradle = if ($IsWindows) { Join-Path $SampleDir "../../gradlew.bat" } else { Join-Path $SampleDir "../../gradlew" }
$Processes = New-Object System.Collections.Generic.List[System.Diagnostics.Process]
$RedisContainer = $null

function Print-Logs {
    param([int]$Status)
    if ($Status -eq 0) { return }
    Get-ChildItem -Path $LogDir -Filter "*.log" -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Host "===== $($_.FullName) ====="
        Get-Content -Path $_.FullName -Tail 200 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
    }
}

function Cleanup {
    param([int]$Status)
    Print-Logs $Status
    for ($i = $Processes.Count - 1; $i -ge 0; $i--) {
        $process = $Processes[$i]
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
    }
    if ($RedisContainer) {
        Remove-ZlinkSampleRedis $RedisContainer
    }
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $ConfigDir
}

function Wait-Port {
    param([string]$HostName, [int]$Port, [int]$TimeoutSeconds = 60)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $client = [System.Net.Sockets.TcpClient]::new()
        try {
            $connect = $client.BeginConnect($HostName, $Port, $null, $null)
            if ($connect.AsyncWaitHandle.WaitOne(200)) {
                $client.EndConnect($connect)
                return
            }
        } catch {
        } finally {
            $client.Close()
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for ${HostName}:$Port"
}

function Split-Endpoint {
    param([string]$Endpoint)
    $parts = $Endpoint.Split(":")
    return @{ Host = $parts[0]; Port = [int]$parts[1] }
}

function Invoke-Gradle {
    param([string[]]$Arguments)
    Invoke-ZlinkSampleGradleBuild -GradleExecutable $Gradle -Arguments $Arguments
}

function Get-AppBin {
    param([string]$Project, [string]$Name)
    $binDir = Join-Path $SampleDir "$Project/build/install/$Name/bin"
    return Join-Path $binDir $(if ($IsWindows) { "$Name.bat" } else { $Name })
}

function Start-AppRole {
    param([string]$Project, [string]$Name, [string]$Config, [string]$LogName)
    $logPath = Join-Path $LogDir $LogName
    $errorLogPath = Join-Path $LogDir ($LogName + ".err.log")
    $process = Start-Process -FilePath (Get-AppBin $Project $Name) -ArgumentList @("--config", $Config) -WorkingDirectory $SampleDir -NoNewWindow -RedirectStandardOutput $logPath -RedirectStandardError $errorLogPath -PassThru
    $Processes.Add($process)
}

function Protect-ConfigFile {
    param([string]$Path)
    if ($IsWindows) {
        $identity = [Security.Principal.WindowsIdentity]::GetCurrent().Name
        & icacls $Path /inheritance:r /grant:r "${identity}:(R,W)" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Could not restrict config file ACL: $Path" }
    } else {
        & chmod 0600 $Path
        if ($LASTEXITCODE -ne 0) { throw "Could not restrict config file mode: $Path" }
    }
}

function Get-LogCount {
    param([string[]]$Path, [string]$Evidence)
    return @(Select-String -Path $Path -Pattern $Evidence -SimpleMatch -ErrorAction SilentlyContinue).Count
}

function Wait-LogCount {
    param([string[]]$Path, [string]$Evidence, [int]$Expected)
    for ($attempt = 0; $attempt -lt 300; $attempt++) {
        $actual = Get-LogCount -Path $Path -Evidence $Evidence
        if ($actual -eq $Expected) { return }
        if ($actual -gt $Expected) {
            throw "Expected $Expected matches for '$Evidence' in $Path, found $actual."
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for $Expected matches for '$Evidence' in $Path."
}

function Assert-LogCount {
    param([string[]]$Path, [string]$Evidence, [int]$Expected)
    $actual = Get-LogCount -Path $Path -Evidence $Evidence
    if ($actual -ne $Expected) {
        throw "Expected $Expected matches for '$Evidence' in $Path, found $actual."
    }
}

$Status = 1
try {
    $endpoints = @(Get-ZlinkSampleApplicationEndpoints -Language Kotlin -Count 13)
    $apiAChannel = Split-Endpoint $endpoints[0]
    $apiAMesh = Split-Endpoint $endpoints[1]
    $sessionARouter = Split-Endpoint $endpoints[2]
    $playARouter = Split-Endpoint $endpoints[3]
    $sessionAStream = Split-Endpoint $endpoints[4]
    $apiBChannel = Split-Endpoint $endpoints[5]
    $apiBMesh = Split-Endpoint $endpoints[6]
    $sessionBRouter = Split-Endpoint $endpoints[7]
    $playBRouter = Split-Endpoint $endpoints[8]
    $sessionBStream = Split-Endpoint $endpoints[9]
    $apiAMatchmaking = Split-Endpoint $endpoints[10]
    $apiBMatchmaking = Split-Endpoint $endpoints[11]
    $matchmakingRouter = Split-Endpoint $endpoints[12]

    $redis = Start-ZlinkSampleRedis "zlink-redis-kotlin-sample-bingo" -Language Kotlin
    $RedisContainer = $redis.ContainerId
    $redisEndpoint = $redis.Endpoint
    $redis = Split-Endpoint $redisEndpoint
    Wait-Port $redis.Host $redis.Port
    $redisKeyPrefix = if ($env:BINGO_REDIS_KEY_PREFIX) { $env:BINGO_REDIS_KEY_PREFIX } else { "bingo:kotlin:${PID}:$([Guid]::NewGuid().ToString('N')):" }
    $commonProperties = @"
sample.api-a-channel-endpoint=tcp://$($apiAChannel.Host):$($apiAChannel.Port)
sample.api-b-channel-endpoint=tcp://$($apiBChannel.Host):$($apiBChannel.Port)
sample.api-a-mesh-endpoint=tcp://$($apiAMesh.Host):$($apiAMesh.Port)
sample.api-b-mesh-endpoint=tcp://$($apiBMesh.Host):$($apiBMesh.Port)
sample.session-a-router-endpoint=tcp://$($sessionARouter.Host):$($sessionARouter.Port)
sample.session-b-router-endpoint=tcp://$($sessionBRouter.Host):$($sessionBRouter.Port)
sample.play-a-spot-router-endpoint=tcp://$($playARouter.Host):$($playARouter.Port)
sample.play-b-spot-router-endpoint=tcp://$($playBRouter.Host):$($playBRouter.Port)
sample.matchmaking-router-endpoint=tcp://$($matchmakingRouter.Host):$($matchmakingRouter.Port)
sample.session-a-stream-endpoint=tcp://$($sessionAStream.Host):$($sessionAStream.Port)
sample.session-b-stream-endpoint=tcp://$($sessionBStream.Host):$($sessionBStream.Port)
sample.redis-endpoint=$redisEndpoint
sample.redis-key-prefix=$redisKeyPrefix
sample.log-directory=$($env:BINGO_LOG_DIR.Replace('\', '/'))
sample.api-node=a
sample.play-node=a
sample.session-node=a
"@
    function Write-SampleConfig {
        param([string]$Name, [string]$RoleName, [string]$RoleValue)
        $path = Join-Path $ConfigDir "$Name.properties"
        $matchmakingEndpoint = if ($RoleName -eq "api-node" -and $RoleValue -eq "b") {
            "tcp://$($apiBMatchmaking.Host):$($apiBMatchmaking.Port)"
        } else {
            "tcp://$($apiAMatchmaking.Host):$($apiAMatchmaking.Port)"
        }
        Set-Content -Path $path -Value "$commonProperties`nsample.api-matchmaking-router-endpoint=$matchmakingEndpoint`nsample.$RoleName=$RoleValue" -Encoding utf8NoBOM
        Protect-ConfigFile $path
        return $path
    }
    $sessionAConfig = Write-SampleConfig "session-a" "session-node" "a"
    $sessionBConfig = Write-SampleConfig "session-b" "session-node" "b"
    $apiAConfig = Write-SampleConfig "api-a" "api-node" "a"
    $apiBConfig = Write-SampleConfig "api-b" "api-node" "b"
    $playAConfig = Write-SampleConfig "play-a" "play-node" "a"
    $playBConfig = Write-SampleConfig "play-b" "play-node" "b"
    $matchmakingConfig = Write-SampleConfig "matchmaking" "matchmaking-node" "matchmaking"
    $clientConfig = Write-SampleConfig "client" "client-node" "client"

    Push-Location "../../.."
    try {
        Invoke-ZlinkSampleGradleBuild -GradleExecutable $Gradle -Arguments @(
            "--no-daemon",
            ":zlink-framework-core:jar",
            ":zlink-framework-spring-boot-starter:jar",
            ":zlink-framework-kotlin:jar",
            ":zlink-framework-locations-redis:jar",
            ":zlink-framework-codec-protobuf:jar",
            ":zlink-stream-connector:jar",
            "--quiet")
    } finally {
        Pop-Location
    }

    Invoke-Gradle @("--settings-file", "standalone.settings.gradle.kts", "--no-daemon", ":Server:Session:installDist", ":Server:Api:installDist", ":Server:Play:installDist", ":Server:Matchmaking:installDist", ":Client:installDist", "--quiet")

    Start-AppRole "Server/Session" "Session" $sessionAConfig "session-a.log"
    Start-AppRole "Server/Matchmaking" "Matchmaking" $matchmakingConfig "matchmaking.log"
    Start-AppRole "Server/Session" "Session" $sessionBConfig "session-b.log"
    Start-AppRole "Server/Api" "Api" $apiAConfig "api-a.log"
    Start-AppRole "Server/Api" "Api" $apiBConfig "api-b.log"
    Start-AppRole "Server/Play" "Play" $playAConfig "play-a.log"
    Start-AppRole "Server/Play" "Play" $playBConfig "play-b.log"

    Wait-Port $sessionARouter.Host $sessionARouter.Port
    Wait-Port $sessionAStream.Host $sessionAStream.Port
    Wait-Port $sessionBRouter.Host $sessionBRouter.Port
    Wait-Port $sessionBStream.Host $sessionBStream.Port
    Wait-Port $apiAChannel.Host $apiAChannel.Port
    Wait-Port $apiBChannel.Host $apiBChannel.Port
    Wait-Port $matchmakingRouter.Host $matchmakingRouter.Port
    Wait-Port $playARouter.Host $playARouter.Port
    Wait-Port $playBRouter.Host $playBRouter.Port

    Wait-LogCount @((Join-Path $LogDir "play-a.log")) "bingo-ready kind=peer-route node=play-a peer=play-b" 1
    Wait-LogCount @((Join-Path $LogDir "play-b.log")) "bingo-ready kind=peer-route node=play-b peer=play-a" 1
    Wait-LogCount @((Join-Path $LogDir "api-a.log")) "bingo-ready kind=mesh-route node=api-a mesh=matchmaking" 1
    Wait-LogCount @((Join-Path $LogDir "api-a.log")) "bingo-ready kind=mesh-route node=api-a mesh=room" 1
    Wait-LogCount @((Join-Path $LogDir "api-b.log")) "bingo-ready kind=mesh-route node=api-b mesh=matchmaking" 1
    Wait-LogCount @((Join-Path $LogDir "api-b.log")) "bingo-ready kind=mesh-route node=api-b mesh=room" 1
    Wait-LogCount @((Join-Path $LogDir "session-a.log")) "bingo-ready kind=mesh-route node=session-a mesh=room" 1
    Wait-LogCount @((Join-Path $LogDir "session-b.log")) "bingo-ready kind=mesh-route node=session-b mesh=room" 1

    $clientLog = Join-Path $LogDir "client.log"
    & (Get-AppBin "Client" "Client") --config $clientConfig *> $clientLog
    if ($LASTEXITCODE -ne 0) {
        throw "Client run failed."
    }
    if (-not (Select-String -Path $clientLog -Pattern "bingo=completed" -Quiet)) {
        throw "Client completion marker was not found."
    }
    if (-not (Select-String -Path $clientLog -Pattern "stream-inbound sample=Bingo" -Quiet)) {
        throw "Client inbound stream evidence was not found."
    }
    if (-not (Select-String -Path (Join-Path $env:BINGO_LOG_DIR "*.log") -Pattern "zlink flow: event_id=zlink.message_flow" -SimpleMatch -Quiet)) {
        throw "Message flow evidence was not found."
    }

    $playLogs = @((Join-Path $LogDir "play-a.log"), (Join-Path $LogDir "play-b.log"))
    $sessionLogs = @((Join-Path $LogDir "session-a.log"), (Join-Path $LogDir "session-b.log"))
    $businessEvidence = @(
        "bingo-record fetched actor=player-1 wins=0 losses=0",
        "bingo-record fetched actor=player-2 wins=0 losses=0",
        "bingo-record reported actor=player-1 wins=1 losses=0",
        "bingo-record reported actor=player-2 wins=0 losses=1",
        "bingo-lifecycle room-leave actor=player-1",
        "bingo-lifecycle room-leave actor=player-2",
        "bingo-lifecycle room-leave actor=observer",
        "bingo-lifecycle entry-leave actor=player-1",
        "bingo-lifecycle entry-leave actor=player-2",
        "bingo-lifecycle entry-leave actor=observer",
        "bingo-lifecycle entry-destroy-complete actor=player-1",
        "bingo-lifecycle entry-destroy-complete actor=player-2"
    )
    foreach ($evidence in $businessEvidence) {
        Wait-LogCount $playLogs $evidence 1
    }
    Wait-LogCount $sessionLogs "bingo-lifecycle session-disconnect actor=player-1 destroy=false" 1
    Wait-LogCount $sessionLogs "bingo-lifecycle session-disconnect actor=player-2 destroy=false" 1
    Wait-LogCount $playLogs "bingo-record reported actor=observer" 0
    Wait-LogCount $playLogs "bingo-lifecycle entry-destroy-complete actor=observer" 0
    foreach ($evidence in $businessEvidence) {
        Assert-LogCount $playLogs $evidence 1
    }
    Assert-LogCount $playLogs "bingo-record reported actor=observer" 0
    Assert-LogCount $playLogs "bingo-lifecycle entry-destroy-complete actor=observer" 0
    Assert-LogCount $sessionLogs "bingo-lifecycle session-disconnect actor=player-1 destroy=false" 1
    Assert-LogCount $sessionLogs "bingo-lifecycle session-disconnect actor=player-2 destroy=false" 1

    $Status = 0
    Write-Output "bingo-placement=completed"
} finally {
    Cleanup $Status
}

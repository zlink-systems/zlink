Set-StrictMode -Version Latest
. "$PSScriptRoot/../../redis-common.ps1"
$ErrorActionPreference = "Stop"

$SampleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $SampleDir

$LogDir = Join-Path $SampleDir "build/sample-logs"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
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
}

function Reserve-Endpoints {
    param([int]$Count)
    $listeners = New-Object System.Collections.Generic.List[System.Net.Sockets.TcpListener]
    $endpoints = New-Object System.Collections.Generic.List[string]
    try {
        while ($endpoints.Count -lt $Count) {
            $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Parse("127.0.0.1"), 0)
            $listener.Start()
            $listeners.Add($listener)
            $endpoints.Add("127.0.0.1:$($listener.LocalEndpoint.Port)")
        }
        return $endpoints.ToArray()
    } finally {
        foreach ($listener in $listeners) {
            $listener.Stop()
        }
    }
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
    & $Gradle @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Gradle failed: $($Arguments -join ' ')"
    }
}

function Start-GradleRole {
    param([string[]]$Arguments, [string]$LogName, [string]$JavaToolOptions)
    $logPath = Join-Path $LogDir $LogName
    $errorLogPath = Join-Path $LogDir ($LogName + ".err.log")
    $previous = $env:JAVA_TOOL_OPTIONS
    try {
        $env:JAVA_TOOL_OPTIONS = $JavaToolOptions
        $process = Start-Process -FilePath $Gradle -ArgumentList $Arguments -WorkingDirectory $SampleDir -NoNewWindow -RedirectStandardOutput $logPath -RedirectStandardError $errorLogPath -PassThru
        $Processes.Add($process)
    } finally {
        $env:JAVA_TOOL_OPTIONS = $previous
    }
}

$Status = 1
$oldJavaToolOptions = $env:JAVA_TOOL_OPTIONS
try {
    $endpoints = Reserve-Endpoints 15
    $apiAChannel = Split-Endpoint $endpoints[0]
    $playAChannel = Split-Endpoint $endpoints[1]
    $sessionASpot = Split-Endpoint $endpoints[2]
    $sessionARouter = Split-Endpoint $endpoints[3]
    $playASpot = Split-Endpoint $endpoints[4]
    $playARouter = Split-Endpoint $endpoints[5]
    $sessionAStream = Split-Endpoint $endpoints[6]
    $apiBChannel = Split-Endpoint $endpoints[7]
    $playBChannel = Split-Endpoint $endpoints[8]
    $sessionBSpot = Split-Endpoint $endpoints[9]
    $sessionBRouter = Split-Endpoint $endpoints[10]
    $playBSpot = Split-Endpoint $endpoints[11]
    $playBRouter = Split-Endpoint $endpoints[12]
    $sessionBStream = Split-Endpoint $endpoints[13]

    $redis = Start-ZlinkSampleRedis "zlink-redis-kotlin-sample-bingo"
    $RedisContainer = $redis.ContainerId
    $redisEndpoint = $redis.Endpoint
    $redis = Split-Endpoint $redisEndpoint
    Wait-Port $redis.Host $redis.Port
    $redisKeyPrefix = if ($env:BINGO_REDIS_KEY_PREFIX) { $env:BINGO_REDIS_KEY_PREFIX } else { "bingo:kotlin:${PID}:$([Guid]::NewGuid().ToString('N')):" }

    $commonJavaOptions = "$oldJavaToolOptions -Dzlink.samples.bingo.apiAChannelEndpoint=tcp://$($apiAChannel.Host):$($apiAChannel.Port) -Dzlink.samples.bingo.apiBChannelEndpoint=tcp://$($apiBChannel.Host):$($apiBChannel.Port) -Dzlink.samples.bingo.playAChannelEndpoint=tcp://$($playAChannel.Host):$($playAChannel.Port) -Dzlink.samples.bingo.playBChannelEndpoint=tcp://$($playBChannel.Host):$($playBChannel.Port) -Dzlink.samples.bingo.sessionASpotEndpoint=tcp://$($sessionASpot.Host):$($sessionASpot.Port) -Dzlink.samples.bingo.sessionBSpotEndpoint=tcp://$($sessionBSpot.Host):$($sessionBSpot.Port) -Dzlink.samples.bingo.sessionARouterEndpoint=tcp://$($sessionARouter.Host):$($sessionARouter.Port) -Dzlink.samples.bingo.sessionBRouterEndpoint=tcp://$($sessionBRouter.Host):$($sessionBRouter.Port) -Dzlink.samples.bingo.playASpotEndpoint=tcp://$($playASpot.Host):$($playASpot.Port) -Dzlink.samples.bingo.playBSpotEndpoint=tcp://$($playBSpot.Host):$($playBSpot.Port) -Dzlink.samples.bingo.playASpotRouterEndpoint=tcp://$($playARouter.Host):$($playARouter.Port) -Dzlink.samples.bingo.playBSpotRouterEndpoint=tcp://$($playBRouter.Host):$($playBRouter.Port) -Dzlink.samples.bingo.sessionAStreamEndpoint=tcp://$($sessionAStream.Host):$($sessionAStream.Port) -Dzlink.samples.bingo.sessionBStreamEndpoint=tcp://$($sessionBStream.Host):$($sessionBStream.Port) -Dzlink.samples.bingo.redisEndpoint=$redisEndpoint -Dzlink.samples.bingo.redisKeyPrefix=$redisKeyPrefix"

    Push-Location "../../.."
    try {
        & ./gradlew --no-daemon :zlink-framework-core:jar :zlink-framework-spring-boot-starter:jar :zlink-framework-kotlin:jar :zlink-framework-locations-redis:jar :zlink-framework-codec-protobuf:jar :zlink-stream-connector:jar --quiet
        if ($LASTEXITCODE -ne 0) { throw "Framework jar build failed" }
    } finally {
        Pop-Location
    }

    Invoke-Gradle @("--settings-file", "standalone.settings.gradle.kts", "--no-daemon", ":Server:Session:installDist", ":Server:Api:installDist", ":Server:Play:installDist", ":Client:installDist", "--quiet")

    Start-GradleRole -Arguments @("--settings-file", "standalone.settings.gradle.kts", "--no-daemon", ":Server:Session:run", "--quiet") -LogName "session-a.log" -JavaToolOptions "$commonJavaOptions -Dzlink.samples.bingo.sessionNode=a"
    Start-GradleRole -Arguments @("--settings-file", "standalone.settings.gradle.kts", "--no-daemon", ":Server:Session:run", "--quiet") -LogName "session-b.log" -JavaToolOptions "$commonJavaOptions -Dzlink.samples.bingo.sessionNode=b"
    Start-GradleRole -Arguments @("--settings-file", "standalone.settings.gradle.kts", "--no-daemon", ":Server:Api:run", "--quiet") -LogName "api-a.log" -JavaToolOptions "$commonJavaOptions -Dzlink.samples.bingo.apiNode=a"
    Start-GradleRole -Arguments @("--settings-file", "standalone.settings.gradle.kts", "--no-daemon", ":Server:Api:run", "--quiet") -LogName "api-b.log" -JavaToolOptions "$commonJavaOptions -Dzlink.samples.bingo.apiNode=b"
    Start-GradleRole -Arguments @("--settings-file", "standalone.settings.gradle.kts", "--no-daemon", ":Server:Play:run", "--quiet") -LogName "play-a.log" -JavaToolOptions "$commonJavaOptions -Dzlink.samples.bingo.playNode=a -Dzlink.samples.bingo.playRid=2201"
    Start-GradleRole -Arguments @("--settings-file", "standalone.settings.gradle.kts", "--no-daemon", ":Server:Play:run", "--quiet") -LogName "play-b.log" -JavaToolOptions "$commonJavaOptions -Dzlink.samples.bingo.playNode=b -Dzlink.samples.bingo.playRid=2202"

    Wait-Port $sessionARouter.Host $sessionARouter.Port
    Wait-Port $sessionAStream.Host $sessionAStream.Port
    Wait-Port $sessionBRouter.Host $sessionBRouter.Port
    Wait-Port $sessionBStream.Host $sessionBStream.Port
    Wait-Port $apiAChannel.Host $apiAChannel.Port
    Wait-Port $apiBChannel.Host $apiBChannel.Port
    Wait-Port $playARouter.Host $playARouter.Port
    Wait-Port $playASpot.Host $playASpot.Port
    Wait-Port $playBRouter.Host $playBRouter.Port
    Wait-Port $playBSpot.Host $playBSpot.Port

    $clientLog = Join-Path $LogDir "client.log"
    $env:JAVA_TOOL_OPTIONS = $commonJavaOptions
    & $Gradle --settings-file standalone.settings.gradle.kts --no-daemon :Client:run --quiet *> $clientLog
    if ($LASTEXITCODE -ne 0) {
        throw "Client run failed."
    }
    if (-not (Select-String -Path $clientLog -Pattern "bingo=completed" -Quiet)) {
        throw "Client completion marker was not found."
    }
    if (-not (Select-String -Path $clientLog -Pattern "stream-inbound sample=Bingo" -Quiet)) {
        throw "Client inbound stream evidence was not found."
    }
    if (-not (Select-String -Path (Join-Path $env:BINGO_LOG_DIR "*.log") -Pattern "message flow" -Quiet)) {
        throw "Message flow evidence was not found."
    }
    $Status = 0
} finally {
    Cleanup $Status
    $env:JAVA_TOOL_OPTIONS = $oldJavaToolOptions
}

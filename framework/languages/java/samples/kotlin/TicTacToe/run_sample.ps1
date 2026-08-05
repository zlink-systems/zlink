Set-StrictMode -Version Latest
. "$PSScriptRoot/../../redis-common.ps1"
$ErrorActionPreference = "Stop"

$SampleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $SampleDir

$RunDir = Join-Path ([System.IO.Path]::GetTempPath()) "zlink-tictactoe-kotlin-$PID-$([Guid]::NewGuid().ToString('N'))"
$LogDir = Join-Path $RunDir "logs"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

$Gradle = if ($IsWindows) { Join-Path $SampleDir "../../gradlew.bat" } else { Join-Path $SampleDir "../../gradlew" }
$Processes = New-Object System.Collections.Generic.List[System.Diagnostics.Process]
$RedisContainer = $null

function Print-Logs {
    param([int]$Status)
    if ($Status -eq 0) { return }
    Get-ChildItem -Path $LogDir -Filter "*.log" -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Error "===== $($_.FullName) ====="
        Get-Content -Path $_.FullName -Tail 240 -ErrorAction SilentlyContinue | ForEach-Object { Write-Error $_ }
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
    if ($env:TICTACTOE_KOTLIN_KEEP_RUN_DIR -eq "1") {
        Write-Host "runDir=$RunDir"
    } else {
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $RunDir
    }
}

function Reserve-Ports {
    param([int]$Count)
    $listeners = New-Object System.Collections.Generic.List[System.Net.Sockets.TcpListener]
    $ports = New-Object System.Collections.Generic.List[int]
    try {
        while ($ports.Count -lt $Count) {
            $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Parse("127.0.0.1"), 0)
            $listener.Start()
            $listeners.Add($listener)
            $ports.Add($listener.LocalEndpoint.Port)
        }
        return $ports.ToArray()
    } finally {
        foreach ($listener in $listeners) {
            $listener.Stop()
        }
    }
}

function Endpoint-Host {
    param([string]$Endpoint)
    $value = $Endpoint -replace '^tcp://', '' -replace '^http://', '' -replace '^redis://', ''
    return ($value -split ':')[0]
}

function Endpoint-Port {
    param([string]$Endpoint)
    $value = $Endpoint -replace '^tcp://', '' -replace '^http://', '' -replace '^redis://', ''
    return [int](($value -split ':')[-1])
}

function Wait-Endpoint {
    param([string]$Name, [string]$Endpoint, [int]$TimeoutSeconds = 15)
    $hostName = Endpoint-Host $Endpoint
    $port = Endpoint-Port $Endpoint
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $client = [System.Net.Sockets.TcpClient]::new()
        try {
            $connect = $client.BeginConnect($hostName, $port, $null, $null)
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
    throw "Timed out waiting for $Name at $Endpoint"
}

function Wait-LogContains {
    param([string]$LogPath, [string]$Pattern, [int]$TimeoutSeconds = 60)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ((Test-Path $LogPath) -and (Select-String -Path $LogPath -Pattern $Pattern -Quiet)) {
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for '$Pattern' in $LogPath"
}

function Invoke-Gradle {
    param([string[]]$Arguments)
    & $Gradle "--no-parallel" "--max-workers=1" @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Gradle failed: $($Arguments -join ' ')"
    }
}

function Start-SampleRole {
    param([string]$Role, [string]$ConfigPath, [string]$LogName)
    $scriptName = if ($Role -eq "play") { "tictactoe-play" } else { "Server" }
    $serverBin = Join-Path $SampleDir "Server/build/install/Server/bin/$scriptName"
    if ($IsWindows) { $serverBin = "$serverBin.bat" }
    $process = Start-Process -FilePath $serverBin -ArgumentList @("--config", $ConfigPath) -WorkingDirectory $SampleDir -NoNewWindow -RedirectStandardOutput (Join-Path $LogDir $LogName) -RedirectStandardError (Join-Path $LogDir "$LogName.err") -PassThru
    $Processes.Add($process)
}

$Status = 1
try {
    $ports = Reserve-Ports 15
    $ApiAHttpPort = $ports[0]
    $ApiBHttpPort = $ports[1]
    $ApiAChannelPort = $ports[2]
    $ApiBChannelPort = $ports[3]
    $PlayAChannelPort = $ports[4]
    $PlayBChannelPort = $ports[5]
    $PlayAStreamPort = $ports[6]
    $PlayBStreamPort = $ports[7]
    $PlayASpotPort = $ports[8]
    $PlayBSpotPort = $ports[9]
    $PlayAPubPort = $ports[12]
    $PlayBPubPort = $ports[13]

    $redis = Start-ZlinkSampleRedis "zlink-redis-kotlin-sample-tictactoe" "redis:7-alpine"
    $RedisContainer = $redis.ContainerId
    $redisEndpoint = $redis.Endpoint
    Wait-Endpoint "redis" $redisEndpoint
    $redisKeyPrefix = "zlink:tictactoe-kotlin:${PID}:$([Guid]::NewGuid().ToString('N')):room:"

    $commonPlayChannels = "tcp://127.0.0.1:${PlayAChannelPort},tcp://127.0.0.1:${PlayBChannelPort}"
    $commonPlayStreams = "tcp://127.0.0.1:${PlayAStreamPort},tcp://127.0.0.1:${PlayBStreamPort}"
    $commonSpots = "tcp://127.0.0.1:${PlayASpotPort},tcp://127.0.0.1:${PlayBSpotPort}"
    $commonPubs = "tcp://127.0.0.1:${PlayAPubPort},tcp://127.0.0.1:${PlayBPubPort}"

    $apiAConfig = Join-Path $RunDir "api-a.properties"
    $apiBConfig = Join-Path $RunDir "api-b.properties"
    $playAConfig = Join-Path $RunDir "play-a.properties"
    $playBConfig = Join-Path $RunDir "play-b.properties"

    @(
        "sample.apiBindUrl=http://127.0.0.1:$ApiAHttpPort",
        "sample.apiPublicUrl=http://127.0.0.1:$ApiAHttpPort",
        "sample.apiChannelEndpoint=tcp://127.0.0.1:$ApiAChannelPort",
        "sample.playChannelEndpoint=tcp://127.0.0.1:$PlayAChannelPort",
        "sample.playChannelEndpoints=$commonPlayChannels",
        "sample.playEndpoint=tcp://127.0.0.1:$PlayAStreamPort",
        "sample.playEndpoints=$commonPlayStreams",
        "sample.spotEndpoint=tcp://127.0.0.1:$PlayASpotPort",
        "sample.routeEndpoint=tcp://127.0.0.1:$PlayASpotPort",
        "sample.spotEndpoints=$commonSpots",
        "sample.spotPubSubEndpoint=tcp://127.0.0.1:$PlayAPubPort",
        "sample.spotPubSubEndpoints=$commonPubs",
        "sample.redisEndpoint=$redisEndpoint",
        "sample.redisKeyPrefix=$redisKeyPrefix",
        "sample.peerSpotEndpoint=tcp://127.0.0.1:$PlayBSpotPort",
        "sample.peerSpotPubSubEndpoint=tcp://127.0.0.1:$PlayBPubPort",
        "sample.logDirectory=$LogDir"
    ) | Set-Content -Path $apiAConfig -Encoding UTF8

    Copy-Item $apiAConfig $apiBConfig
    (Get-Content $apiBConfig) `
        -replace 'sample\.apiBindUrl=.*', "sample.apiBindUrl=http://127.0.0.1:$ApiBHttpPort" `
        -replace 'sample\.apiPublicUrl=.*', "sample.apiPublicUrl=http://127.0.0.1:$ApiBHttpPort" `
        -replace 'sample\.apiChannelEndpoint=.*', "sample.apiChannelEndpoint=tcp://127.0.0.1:$ApiBChannelPort" |
        Set-Content -Path $apiBConfig -Encoding UTF8

    Copy-Item $apiAConfig $playAConfig
    Copy-Item $apiAConfig $playBConfig
    (Get-Content $playBConfig) `
        -replace 'sample\.playChannelEndpoint=.*', "sample.playChannelEndpoint=tcp://127.0.0.1:$PlayBChannelPort" `
        -replace 'sample\.playEndpoint=.*', "sample.playEndpoint=tcp://127.0.0.1:$PlayBStreamPort" `
        -replace 'sample\.spotEndpoint=.*', "sample.spotEndpoint=tcp://127.0.0.1:$PlayBSpotPort" `
        -replace 'sample\.routeEndpoint=.*', "sample.routeEndpoint=tcp://127.0.0.1:$PlayBSpotPort" `
        -replace 'sample\.spotPubSubEndpoint=.*', "sample.spotPubSubEndpoint=tcp://127.0.0.1:$PlayBPubPort" `
        -replace 'sample\.peerSpotEndpoint=.*', "sample.peerSpotEndpoint=tcp://127.0.0.1:$PlayASpotPort" `
        -replace 'sample\.peerSpotPubSubEndpoint=.*', "sample.peerSpotPubSubEndpoint=tcp://127.0.0.1:$PlayAPubPort" |
        Set-Content -Path $playBConfig -Encoding UTF8

    Invoke-Gradle @("--settings-file", "standalone.settings.gradle.kts", "--no-daemon", ":Server:installDist", ":Client:installDist", "--quiet")

    Start-SampleRole "play" $playBConfig "play-b.log"
    Wait-LogContains (Join-Path $LogDir "play-b.log") "Started PlayProgram"
    Wait-Endpoint "play-b-stream" "tcp://127.0.0.1:$PlayBStreamPort"
    Wait-Endpoint "play-b-spot" "tcp://127.0.0.1:$PlayBSpotPort"

    Start-SampleRole "play" $playAConfig "play-a.log"
    Wait-LogContains (Join-Path $LogDir "play-a.log") "Started PlayProgram"
    Wait-Endpoint "play-a-stream" "tcp://127.0.0.1:$PlayAStreamPort"
    Wait-Endpoint "play-a-spot" "tcp://127.0.0.1:$PlayASpotPort"

    Start-SampleRole "api" $apiAConfig "api-a.log"
    Wait-LogContains (Join-Path $LogDir "api-a.log") "Started ApiProgram"
    Wait-Endpoint "api-a-http" "http://127.0.0.1:$ApiAHttpPort"

    Start-SampleRole "api" $apiBConfig "api-b.log"
    Wait-LogContains (Join-Path $LogDir "api-b.log") "Started ApiProgram"
    Wait-Endpoint "api-b-http" "http://127.0.0.1:$ApiBHttpPort"

    $clientBin = Join-Path $SampleDir "Client/build/install/Client/bin/Client"
    if ($IsWindows) { $clientBin = "$clientBin.bat" }
    $clientLog = Join-Path $LogDir "client.log"
    & $clientBin --api-url "http://127.0.0.1:$ApiAHttpPort" *> $clientLog
    if ($LASTEXITCODE -ne 0) {
        throw "Client run failed."
    }
    if (-not (Select-String -Path $clientLog -Pattern "observer-connected endpoint=tcp://127.0.0.1:$PlayBStreamPort" -Quiet)) {
        throw "Observer connection marker was not found."
    }
    if (-not (Select-String -Path $clientLog -Pattern "observer-subscription=verified subscribed=true" -Quiet)) {
        throw "Observer subscription marker was not found."
    }
    if (-not (Select-String -Path $clientLog -Pattern "observer-win-milestone=verified actor=player-x wins=100" -Quiet)) {
        throw "Observer milestone marker was not found."
    }
    if (-not (Select-String -Path $clientLog -Pattern "tictactoe completed" -Quiet)) {
        throw "Completion marker was not found."
    }
    if (-not (Select-String -Path (Join-Path $FlowLogDir "*.log") -Pattern "message flow" -Quiet -ErrorAction SilentlyContinue)) {
        throw "Message flow evidence was not found."
    }
    Write-Host "PASS TicTacToe.Kotlin"
    $Status = 0
} finally {
    Cleanup $Status
}

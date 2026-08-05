Set-StrictMode -Version Latest
. "$PSScriptRoot/../../redis-common.ps1"
$ErrorActionPreference = "Stop"

$SampleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $SampleDir

$RunDir = Join-Path ([IO.Path]::GetTempPath()) ("zlink-tictactoe-" + [Guid]::NewGuid().ToString("N"))
$LogDir = Join-Path $RunDir "logs"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $LogDir "*.log")

$Gradle = if ($IsWindows) { Join-Path $SampleDir "../../gradlew.bat" } else { Join-Path $SampleDir "../../gradlew" }
$Processes = New-Object System.Collections.Generic.List[System.Diagnostics.Process]
$RedisContainer = $null

function Print-Logs {
    param([int]$Status)
    if ($Status -eq 0) { return }
    Get-ChildItem -Path $LogDir -Filter "*.log" -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Error "===== $($_.FullName) ====="
        Get-Content -Path $_.FullName -Tail 200 -ErrorAction SilentlyContinue | ForEach-Object { Write-Error $_ }
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
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $RunDir
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

function Wait-Port {
    param([int]$Port, [int]$TimeoutSeconds = 45)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $client = [System.Net.Sockets.TcpClient]::new()
        try {
            $connect = $client.BeginConnect("127.0.0.1", $Port, $null, $null)
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
    throw "Timed out waiting for port $Port"
}

function Invoke-Gradle {
    param([string[]]$Arguments)
    & $Gradle @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Gradle failed: $($Arguments -join ' ')"
    }
}

function Start-SampleRole {
    param([string]$Role, [string]$ConfigPath, [string]$LogName)
    $logPath = Join-Path $LogDir $LogName
    $errorLogPath = Join-Path $LogDir ($LogName + ".err.log")
    $scriptName = if ($Role -eq "play") { "tictactoe-play" } else { "Server" }
    $serverBin = Join-Path $SampleDir "Server/build/install/Server/bin/$scriptName"
    if ($IsWindows) { $serverBin = "$serverBin.bat" }
    $process = Start-Process -FilePath $serverBin -ArgumentList @("--config", $ConfigPath) -WorkingDirectory $SampleDir -NoNewWindow -RedirectStandardOutput $logPath -RedirectStandardError $errorLogPath -PassThru
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

$Status = 1
try {
    $ports = Reserve-Ports 12
    $ApiAPort = $ports[0]
    $ApiBPort = $ports[1]
    $ApiAChannelPort = $ports[2]
    $ApiBChannelPort = $ports[3]
    $PlayAStreamPort = $ports[4]
    $PlayBStreamPort = $ports[5]
    $PlayAChannelPort = $ports[6]
    $PlayBChannelPort = $ports[7]
    $PlayASpotPort = $ports[8]
    $PlayBSpotPort = $ports[9]
    $PlayAPubPort = $ports[10]
    $PlayBPubPort = $ports[11]
    $redis = Start-ZlinkSampleRedis "zlink-redis-java-sample-tictactoe" "redis:7-alpine"
    $RedisContainer = $redis.ContainerId
    $RedisEndpoint = $redis.Endpoint
    $RedisKeyPrefix = "zlink:tictactoe:${PID}:$([Guid]::NewGuid().ToString('N')):room:"

    $PlayChannels = "tcp://127.0.0.1:$PlayAChannelPort,tcp://127.0.0.1:$PlayBChannelPort"
    $PlayStreams = "tcp://127.0.0.1:$PlayAStreamPort,tcp://127.0.0.1:$PlayBStreamPort"
    function Write-ApiConfig {
        param([string]$Name, [int]$HttpPort, [int]$ChannelPort)
        $path = Join-Path $RunDir "$Name.properties"
        @(
            "sample.apiBindUrl=http://127.0.0.1:$HttpPort",
            "sample.apiChannelEndpoint=tcp://127.0.0.1:$ChannelPort",
            "sample.playChannelEndpoint=tcp://127.0.0.1:$PlayAChannelPort",
            "sample.playChannelEndpoints=$PlayChannels",
            "sample.logDirectory=$LogDir"
        ) | Set-Content -Path $path -Encoding utf8NoBOM
        Protect-ConfigFile $path
        return $path
    }
    function Write-PlayConfig {
        param(
            [string]$Name,
            [int]$ChannelPort,
            [int]$StreamPort,
            [int]$SpotPort,
            [int]$PubPort,
            [int]$PeerSpotPort,
            [int]$PeerPubPort)
        $path = Join-Path $RunDir "$Name.properties"
        @(
        "sample.apiChannelEndpoint=tcp://127.0.0.1:$ApiAChannelPort",
        "sample.playChannelEndpoint=tcp://127.0.0.1:$ChannelPort",
        "sample.playEndpoint=tcp://127.0.0.1:$StreamPort",
        "sample.playEndpoints=$PlayStreams",
        "sample.spotEndpoint=tcp://127.0.0.1:$SpotPort",
        "sample.spotPubSubEndpoint=tcp://127.0.0.1:$PubPort",
        "sample.redisEndpoint=$RedisEndpoint",
        "sample.redisKeyPrefix=$RedisKeyPrefix",
        "sample.peerSpotEndpoint=tcp://127.0.0.1:$PeerSpotPort",
        "sample.peerSpotPubSubEndpoint=tcp://127.0.0.1:$PeerPubPort",
        "sample.logDirectory=$LogDir"
        ) | Set-Content -Path $path -Encoding utf8NoBOM
        Protect-ConfigFile $path
        return $path
    }
    $ApiAConfig = Write-ApiConfig "api-a" $ApiAPort $ApiAChannelPort
    $ApiBConfig = Write-ApiConfig "api-b" $ApiBPort $ApiBChannelPort
    $PlayAConfig = Write-PlayConfig "play-a" $PlayAChannelPort $PlayAStreamPort `
        $PlayASpotPort $PlayAPubPort $PlayBSpotPort $PlayBPubPort
    $PlayBConfig = Write-PlayConfig "play-b" $PlayBChannelPort $PlayBStreamPort `
        $PlayBSpotPort $PlayBPubPort $PlayASpotPort $PlayAPubPort

    Invoke-Gradle @("--settings-file", "standalone.settings.gradle.kts", ":Server:installDist", ":Client:installDist", "--quiet")

    Start-SampleRole "play" $PlayBConfig "play-b.log"
    Wait-Port $PlayBStreamPort
    Wait-Port $PlayBChannelPort
    Start-SampleRole "play" $PlayAConfig "play-a.log"
    Wait-Port $PlayAStreamPort
    Wait-Port $PlayAChannelPort

    Start-SampleRole "api" $ApiAConfig "api-a.log"
    Wait-Port $ApiAPort
    Wait-Port $ApiAChannelPort
    Start-SampleRole "api" $ApiBConfig "api-b.log"
    Wait-Port $ApiBPort
    Wait-Port $ApiBChannelPort

    Invoke-Gradle @("--settings-file", "standalone.settings.gradle.kts", ":Client:run", "--quiet", "--args=--api-url http://127.0.0.1:$ApiAPort")
    Write-Host "PASS TicTacToe.Java"
    $Status = 0
} finally {
    Cleanup $Status
}

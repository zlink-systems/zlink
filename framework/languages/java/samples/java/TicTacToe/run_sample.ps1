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
        [Console]::Error.WriteLine("===== $($_.FullName) =====")
        Get-Content -Path $_.FullName -Tail 200 -ErrorAction SilentlyContinue | ForEach-Object { [Console]::Error.WriteLine($_) }
    }
}

function Cleanup {
    param([int]$Status)
    Print-Logs $Status
    for ($i = $Processes.Count - 1; $i -ge 0; $i--) {
        Stop-ZlinkSampleProcessTree -Process $Processes[$i] -Force
    }
    if ($RedisContainer) {
        Remove-ZlinkSampleRedis $RedisContainer
    }
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $RunDir
}

function Wait-Port {
    param([int]$Port)
    for ($attempt = 0; $attempt -lt 300; $attempt++) {
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

function Wait-LogCount {
    param(
        [string]$PathPattern,
        [string]$Text,
        [int]$Expected)
    for ($attempt = 0; $attempt -lt 300; $attempt++) {
        $count = @(Select-String -Path $PathPattern -Pattern $Text -SimpleMatch -ErrorAction SilentlyContinue).Count
        if ($count -eq $Expected) {
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for $Expected '$Text' in $PathPattern"
}

function Invoke-GradleRun {
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
    Register-ZlinkSampleProcessTree -Process $process
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
    $ports = @(Get-ZlinkSampleApplicationPorts -Language Java -Count 12)
    $ApiAPort = $ports[0]
    $ApiBPort = $ports[1]
    $ApiAChannelPort = $ports[2]
    $ApiBChannelPort = $ports[3]
    $PlayAStreamPort = $ports[4]
    $PlayBStreamPort = $ports[5]
    $ApiARoutePort = $ports[6]
    $ApiBRoutePort = $ports[7]
    $PlayASpotPort = $ports[8]
    $PlayBSpotPort = $ports[9]
    $PlayAPubPort = $ports[10]
    $PlayBPubPort = $ports[11]
    $redis = Start-ZlinkSampleRedis "zlink-redis-java-sample-tictactoe" `
        "redis:7-alpine" -Language Java
    $RedisContainer = $redis.ContainerId
    $RedisEndpoint = $redis.Endpoint
    $RedisKeyPrefix = "zlink:tictactoe:${PID}:$([Guid]::NewGuid().ToString('N')):room:"

    $ApiChannels = "tcp://127.0.0.1:$ApiAChannelPort,tcp://127.0.0.1:$ApiBChannelPort"
    $PlayStreams = "tcp://127.0.0.1:$PlayAStreamPort,tcp://127.0.0.1:$PlayBStreamPort"
    $Spots = "tcp://127.0.0.1:$PlayASpotPort,tcp://127.0.0.1:$PlayBSpotPort"
    function Write-ApiConfig {
        param([string]$Name, [int]$HttpPort, [int]$ChannelPort, [int]$RoutePort)
        $path = Join-Path $RunDir "$Name.properties"
        Set-ZlinkSampleUtf8File -Path $path -Value @(
            "sample.nodeId=$Name",
            "sample.apiBindUrl=http://127.0.0.1:$HttpPort",
            "sample.apiChannelEndpoint=tcp://127.0.0.1:$ChannelPort",
            "sample.playEndpoints=$PlayStreams",
            "sample.routeEndpoint=tcp://127.0.0.1:$RoutePort",
            "sample.spotEndpoints=$Spots",
            "sample.redisEndpoint=$RedisEndpoint",
            "sample.redisKeyPrefix=$RedisKeyPrefix",
            "sample.logDirectory=$LogDir"
        )
        Protect-ConfigFile $path
        return $path
    }
    function Write-PlayConfig {
        param(
            [string]$Name,
            [int]$StreamPort,
            [int]$SpotPort,
            [int]$PubPort,
            [int]$PeerSpotPort,
            [int]$PeerPubPort)
        $path = Join-Path $RunDir "$Name.properties"
        Set-ZlinkSampleUtf8File -Path $path -Value @(
        "sample.nodeId=$Name",
        "sample.apiChannelEndpoints=$ApiChannels",
        "sample.playEndpoint=tcp://127.0.0.1:$StreamPort",
        "sample.playEndpoints=$PlayStreams",
        "sample.spotEndpoint=tcp://127.0.0.1:$SpotPort",
        "sample.spotPubSubEndpoint=tcp://127.0.0.1:$PubPort",
        "sample.redisEndpoint=$RedisEndpoint",
        "sample.redisKeyPrefix=$RedisKeyPrefix",
        "sample.peerSpotEndpoint=tcp://127.0.0.1:$PeerSpotPort",
        "sample.peerSpotPubSubEndpoint=tcp://127.0.0.1:$PeerPubPort",
        "sample.logDirectory=$LogDir"
        )
        Protect-ConfigFile $path
        return $path
    }
    $ApiAConfig = Write-ApiConfig "api-a" $ApiAPort $ApiAChannelPort $ApiARoutePort
    $ApiBConfig = Write-ApiConfig "api-b" $ApiBPort $ApiBChannelPort $ApiBRoutePort
    $PlayAConfig = Write-PlayConfig "play-a" $PlayAStreamPort `
        $PlayASpotPort $PlayAPubPort $PlayBSpotPort $PlayBPubPort
    $PlayBConfig = Write-PlayConfig "play-b" $PlayBStreamPort `
        $PlayBSpotPort $PlayBPubPort $PlayASpotPort $PlayAPubPort

    Invoke-ZlinkSampleGradleBuild -GradleExecutable $Gradle -Arguments @(
        "--settings-file",
        "standalone.settings.gradle.kts",
        "--no-daemon",
        "--no-parallel",
        "--max-workers=1",
        ":Server:installDist",
        ":Client:installDist",
        "--quiet")

    Start-SampleRole "play" $PlayBConfig "play-b.log"
    Wait-Port $PlayBStreamPort
    Wait-Port $PlayBSpotPort
    Start-SampleRole "play" $PlayAConfig "play-a.log"
    Wait-Port $PlayAStreamPort
    Wait-Port $PlayASpotPort

    Start-SampleRole "api" $ApiAConfig "api-a.log"
    Wait-Port $ApiAPort
    Start-SampleRole "api" $ApiBConfig "api-b.log"
    Wait-Port $ApiBPort

    Wait-LogCount (Join-Path $LogDir "play-a.log") "tictactoe-ready kind=peer-route node=play-a peer=play-b" 1
    Wait-LogCount (Join-Path $LogDir "play-b.log") "tictactoe-ready kind=peer-route node=play-b peer=play-a" 1
    Wait-LogCount (Join-Path $LogDir "api-a.log") "tictactoe-ready kind=http node=api-a" 1
    Wait-LogCount (Join-Path $LogDir "api-b.log") "tictactoe-ready kind=http node=api-b" 1
    Wait-LogCount (Join-Path $LogDir "api-a.log") "tictactoe-ready kind=spot-route node=api-a mesh=tictactoe" 1
    Wait-LogCount (Join-Path $LogDir "api-b.log") "tictactoe-ready kind=spot-route node=api-b mesh=tictactoe" 1

    foreach ($TargetRid in @("tictactoe-play-a", "tictactoe-play-b")) {
        $ready = $false
        for ($attempt = 0; $attempt -lt 300; $attempt++) {
            try {
                Invoke-WebRequest -UseBasicParsing -TimeoutSec 1 `
                    -Uri "http://127.0.0.1:$ApiAPort/ready?targetRid=$TargetRid" | Out-Null
                $ready = $true
                break
            } catch {
                Start-Sleep -Milliseconds 100
            }
        }
        if (-not $ready) { throw "Timed out waiting for API route peer $TargetRid" }
    }

    $clientBin = Join-Path $SampleDir "Client/build/install/Client/bin/Client"
    if ($IsWindows) { $clientBin = "$clientBin.bat" }
    $clientLog = Join-Path $LogDir "client.log"
    $clientErrorLog = Join-Path $LogDir "client.err.log"
    $lifecycleCompletionFile = Join-Path $RunDir "lifecycle-complete"
    $clientProcess = Start-Process -FilePath $clientBin `
        -ArgumentList @(
            "--api-url", "http://127.0.0.1:$ApiAPort",
            "--lifecycle-completion-file", "`"$lifecycleCompletionFile`"") `
        -WorkingDirectory $SampleDir -NoNewWindow `
        -RedirectStandardOutput $clientLog -RedirectStandardError $clientErrorLog -PassThru
    $Processes.Add($clientProcess)
    Register-ZlinkSampleProcessTree -Process $clientProcess
    $PlayLogs = Join-Path $LogDir "play-*.log"
    Wait-LogCount $PlayLogs "tictactoe-lifecycle actor-bound actor=player-x" 1
    foreach ($ActorId in @("player-x", "player-o")) {
        Wait-LogCount $PlayLogs "tictactoe-lifecycle leave-completed actor=$ActorId" 1
        Wait-LogCount $PlayLogs "tictactoe-lifecycle actor-destroy-complete actor=$ActorId" 1
    }
    New-Item -ItemType File -Path $lifecycleCompletionFile | Out-Null
    $clientProcess.WaitForExit()
    if ($clientProcess.ExitCode -ne 0) { throw "Client run failed." }
    Wait-LogCount $clientLog "observer-connected endpoint=tcp://127.0.0.1:$PlayBStreamPort" 1
    Wait-LogCount $clientLog "observer-subscription=verified subscribed=true" 1
    Wait-LogCount $clientLog "observer-win-milestone=verified actor=player-x wins=100" 1
    if (@(Select-String -Path $clientLog -Pattern "reconnected-game-state=verified actor=player-x room=" -SimpleMatch).Count -ne 1) {
        throw "Expected one reconnected game state marker."
    }
    Wait-LogCount $clientLog "tictactoe=completed" 1
    Wait-LogCount $PlayLogs "tictactoe-lifecycle actor-destroy-complete actor=observer" 0
    Write-Host "tictactoe-placement=completed"
    $Status = 0
} finally {
    Cleanup $Status
}

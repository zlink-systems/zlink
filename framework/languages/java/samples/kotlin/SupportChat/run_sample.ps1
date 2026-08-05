Set-StrictMode -Version Latest
. "$PSScriptRoot/../../redis-common.ps1"
$ErrorActionPreference = "Stop"

$SampleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $SampleDir

$RunDir = Join-Path ([System.IO.Path]::GetTempPath()) "supportchat-kotlin-$PID-$([Guid]::NewGuid().ToString('N'))"
$LogDir = Join-Path $RunDir "logs"
$SampleLogDir = Join-Path $RunDir "sample-logs"
$BuildLog = Join-Path $LogDir "build.log"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
New-Item -ItemType Directory -Force -Path $SampleLogDir | Out-Null
$RedisKeyPrefix = "supportchat:kotlin:${PID}:$([Guid]::NewGuid().ToString('N')):"

$Gradle = if ($IsWindows) { Join-Path $SampleDir "../../gradlew.bat" } else { Join-Path $SampleDir "../../gradlew" }
$Processes = New-Object System.Collections.Generic.List[System.Diagnostics.Process]
$RedisContainer = $null
$Status = 1

function Cleanup {
    param([int]$ExitStatus)
    if ($ExitStatus -ne 0) {
        Get-ChildItem -Path $LogDir -Filter "*.log" -ErrorAction SilentlyContinue | ForEach-Object {
            Write-Host "===== $($_.FullName) ====="
            Get-Content -Path $_.FullName -Tail 200 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
        }
    }
    for ($i = $Processes.Count - 1; $i -ge 0; $i--) {
        $process = $Processes[$i]
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
        }
    }
    foreach ($process in $Processes) {
        try {
            $process.WaitForExit(2000) | Out-Null
        } catch {
        }
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
    }
    if ($RedisContainer) {
        Remove-ZlinkSampleRedis $RedisContainer
    }
    if ($env:SUPPORTCHAT_KEEP_RUN_DIR -eq "1") {
        Write-Host "runDir=$RunDir"
    } else {
        Remove-Item -Recurse -Force $RunDir -ErrorAction SilentlyContinue
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

function Wait-Port {
    param([string]$Name, [string]$Endpoint, [int]$TimeoutSeconds = 60)
    $address = $Endpoint -replace '^tcp://', ''
    $parts = $address.Split(':')
    $hostName = $parts[0]
    $port = [int]$parts[1]
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

function Wait-Log {
    param([string]$Pattern, [string]$Path, [int]$Attempts = 60)
    for ($i = 0; $i -lt $Attempts; $i++) {
        if ((Test-Path $Path) -and (Select-String -Path $Path -Pattern $Pattern -Quiet)) {
            return
        }
        Start-Sleep -Milliseconds 200
    }
    throw "Timed out waiting for '$Pattern' in $Path"
}

function Start-Role {
    param([string]$Name, [string]$Binary, [string]$ConfigPath)
    $logPath = Join-Path $LogDir "$Name.log"
    $errPath = Join-Path $LogDir "$Name.err.log"
    $process = Start-Process -FilePath $Binary -ArgumentList @("--config", $ConfigPath) -WorkingDirectory $SampleDir -NoNewWindow -RedirectStandardOutput $logPath -RedirectStandardError $errPath -PassThru
    $Processes.Add($process)
}

try {
    $ports = Reserve-Ports 6
    $ApiChannelEndpoint = "tcp://127.0.0.1:$($ports[0])"
    $ApiHttpEndpoint = "http://127.0.0.1:$($ports[1])"
    $SupportChannelEndpoint = "tcp://127.0.0.1:$($ports[2])"
    $SessionRouterEndpoint = "tcp://127.0.0.1:$($ports[3])"
    $SupportRouterEndpoint = "tcp://127.0.0.1:$($ports[4])"
    $StreamEndpoint = "tcp://127.0.0.1:$($ports[5])"

    $redis = Start-ZlinkSampleRedis "zlink-redis-kotlin-sample-supportchat"
    $RedisContainer = $redis.ContainerId
    $RedisEndpoint = $redis.Endpoint
    Wait-Port "redis" "tcp://$RedisEndpoint"

    $ApiConfig = Join-Path $RunDir "api.properties"
    $SessionConfig = Join-Path $RunDir "session.properties"
    $SupportConfig = Join-Path $RunDir "support.properties"
    @(
        "sample.redisEndpoint=$RedisEndpoint",
        "sample.redisKeyPrefix=$RedisKeyPrefix",
        "sample.logDirectory=$SampleLogDir",
        "sample.apiChannelEndpoint=$ApiChannelEndpoint",
        "sample.apiHttpEndpoint=$ApiHttpEndpoint"
    ) | Set-Content -Path $ApiConfig -Encoding UTF8
    @(
        "sample.redisEndpoint=$RedisEndpoint",
        "sample.redisKeyPrefix=$RedisKeyPrefix",
        "sample.logDirectory=$SampleLogDir",
        "sample.sessionRouterEndpoint=$SessionRouterEndpoint",
        "sample.streamEndpoint=$StreamEndpoint"
    ) | Set-Content -Path $SessionConfig -Encoding UTF8
    @(
        "sample.redisEndpoint=$RedisEndpoint",
        "sample.redisKeyPrefix=$RedisKeyPrefix",
        "sample.logDirectory=$SampleLogDir",
        "sample.supportChannelEndpoint=$SupportChannelEndpoint",
        "sample.supportSpotRouterEndpoint=$SupportRouterEndpoint"
    ) | Set-Content -Path $SupportConfig -Encoding UTF8

    & $Gradle --settings-file standalone.settings.gradle.kts --no-daemon --no-parallel --max-workers=1 `
        :Server:Api:installDist `
        :Server:Session:installDist `
        :Server:Support:installDist `
        :Client:installDist *> $BuildLog
    if ($LASTEXITCODE -ne 0) {
        throw "Gradle installDist failed."
    }

    Start-Role "support" (Join-Path $SampleDir "Server/Support/build/install/Support/bin/Support") $SupportConfig
    Wait-Port "support-channel" $SupportChannelEndpoint
    Wait-Port "support-router" $SupportRouterEndpoint

    Start-Role "api" (Join-Path $SampleDir "Server/Api/build/install/Api/bin/Api") $ApiConfig
    Wait-Port "api-channel" $ApiChannelEndpoint

    Start-Role "session" (Join-Path $SampleDir "Server/Session/build/install/Session/bin/Session") $SessionConfig
    Wait-Port "session-router" $SessionRouterEndpoint
    Wait-Port "session-stream" $StreamEndpoint

    $clientLog = Join-Path $LogDir "client.log"
    & (Join-Path $SampleDir "Client/build/install/Client/bin/Client") `
        --stream-endpoint $StreamEndpoint *> $clientLog
    if ($LASTEXITCODE -ne 0) {
        throw "SupportChat client failed."
    }
    if (-not (Select-String -Path $clientLog -Pattern "supportchat=completed" -Quiet)) {
        throw "SupportChat client did not complete."
    }
    if (-not (Select-String -Path $clientLog -Pattern "supportchat-closed-typing-ignore=verified" -Quiet)) {
        throw "SupportChat client did not verify closed typing ignore."
    }

    Wait-Log "support conversation: created" (Join-Path $LogDir "support.log")
    Wait-Log "support conversation: actor joined" (Join-Path $LogDir "support.log")
    Wait-Log "status=WaitingForAgent" (Join-Path $LogDir "support.log")
    Wait-Log "status=Active" (Join-Path $LogDir "support.log")
    Wait-Log "status=WaitingForClose" (Join-Path $LogDir "support.log")
    Wait-Log "status=Closed" (Join-Path $LogDir "support.log")
    if (-not (Select-String -Path (Join-Path $SampleLogDir "*.log") -Pattern "message flow" -Quiet)) {
        throw "SupportChat message-flow evidence was not found."
    }
    Write-Host "supportchat-server-evidence=completed"
    $Status = 0
} finally {
    Cleanup $Status
}

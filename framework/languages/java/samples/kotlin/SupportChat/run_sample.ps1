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

function Get-LogCount {
    param([string[]]$Paths, [string]$Evidence)
    return @(Select-String -Path $Paths -Pattern $Evidence -SimpleMatch -ErrorAction SilentlyContinue).Count
}

function Wait-LogCount {
    param([string[]]$Paths, [string]$Evidence, [int]$Expected)
    for ($attempt = 0; $attempt -lt 300; $attempt++) {
        $actual = Get-LogCount $Paths $Evidence
        if ($actual -eq $Expected) { return }
        if ($actual -gt $Expected) { throw "Expected $Expected '$Evidence', found $actual." }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for $Expected '$Evidence'."
}

function Wait-LogAtLeast {
    param([string[]]$Paths, [string]$Evidence, [int]$Minimum)
    for ($attempt = 0; $attempt -lt 300; $attempt++) {
        if ((Get-LogCount $Paths $Evidence) -ge $Minimum) { return }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for at least $Minimum '$Evidence'."
}

function Start-Role {
    param([string]$Name, [string]$Binary, [string]$ConfigPath)
    $logPath = Join-Path $LogDir "$Name.log"
    $errPath = Join-Path $LogDir "$Name.err.log"
    $process = Start-Process -FilePath $Binary -ArgumentList @("--config", $ConfigPath) -WorkingDirectory $SampleDir -NoNewWindow -RedirectStandardOutput $logPath -RedirectStandardError $errPath -PassThru
    $Processes.Add($process)
}

try {
    $ports = @(Get-ZlinkSampleApplicationPorts -Language Kotlin -Count 7)
    $ApiChannelEndpoint = "tcp://127.0.0.1:$($ports[0])"
    $ApiHttpEndpoint = "http://127.0.0.1:$($ports[1])"
    $ApiRouterEndpoint = "tcp://127.0.0.1:$($ports[2])"
    $SupportChannelEndpoint = "tcp://127.0.0.1:$($ports[3])"
    $SessionRouterEndpoint = "tcp://127.0.0.1:$($ports[4])"
    $SupportRouterEndpoint = "tcp://127.0.0.1:$($ports[5])"
    $StreamEndpoint = "tcp://127.0.0.1:$($ports[6])"

    $redis = Start-ZlinkSampleRedis "zlink-redis-kotlin-sample-supportchat" -Language Kotlin
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
        "sample.apiSpotRouterEndpoint=$ApiRouterEndpoint",
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

    Invoke-ZlinkSampleGradleBuild -GradleExecutable $Gradle -Arguments @(
        "--settings-file",
        "standalone.settings.gradle.kts",
        "--no-daemon",
        "--no-parallel",
        "--max-workers=1",
        ":Server:Api:installDist",
        ":Server:Session:installDist",
        ":Server:Support:installDist",
        ":Client:installDist") *> $BuildLog

    Start-Role "support" (Join-Path $SampleDir "Server/Support/build/install/Support/bin/Support") $SupportConfig
    Start-Role "api" (Join-Path $SampleDir "Server/Api/build/install/Api/bin/Api") $ApiConfig
    Start-Role "session" (Join-Path $SampleDir "Server/Session/build/install/Session/bin/Session") $SessionConfig

    Wait-LogCount @((Join-Path $LogDir "api.log")) "supportchat-ready kind=public node=api" 1
    Wait-LogCount @((Join-Path $LogDir "support.log")) "supportchat-ready kind=public node=support" 1
    Wait-LogCount @((Join-Path $LogDir "session.log")) "supportchat-ready kind=stream node=session" 1
    Wait-LogCount @((Join-Path $LogDir "api.log")) "supportchat-ready kind=spot-route node=api mesh=supportchat.support.spots" 1
    Wait-LogCount @((Join-Path $LogDir "session.log")) "supportchat-ready kind=spot-route node=session mesh=supportchat.support.spots" 1

    $clientLog = Join-Path $LogDir "client.log"
    & (Join-Path $SampleDir "Client/build/install/Client/bin/Client") `
        --stream-endpoint $StreamEndpoint *> $clientLog
    if ($LASTEXITCODE -ne 0) {
        throw "SupportChat client failed."
    }
    Wait-LogCount @($clientLog) "supportchat=completed" 1
    Wait-LogCount @($clientLog) "supportchat-closed-typing-ignore=verified" 1

    $serverLogs = @((Join-Path $LogDir "api.log"), (Join-Path $LogDir "support.log"))
    Wait-LogAtLeast $serverLogs "supportchat-conversation created conversation=" 1
    Wait-LogAtLeast $serverLogs "supportchat-conversation agent-joined conversation=" 1
    Wait-LogAtLeast $serverLogs "supportchat-conversation status=WaitingForAgent conversation=" 1
    Wait-LogAtLeast $serverLogs "supportchat-conversation status=Active conversation=" 1
    Wait-LogAtLeast $serverLogs "supportchat-conversation status=WaitingForClose conversation=" 1
    Wait-LogAtLeast $serverLogs "supportchat-conversation status=Closed conversation=" 1
    $Status = 0
} finally {
    Cleanup $Status
}
Write-Host "supportchat-placement=completed"

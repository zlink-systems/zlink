Set-StrictMode -Version Latest
. "$PSScriptRoot/../../redis-common.ps1"
$ErrorActionPreference = "Stop"

$SampleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $SampleDir

$RunDir = Join-Path ([System.IO.Path]::GetTempPath()) "zlink-deliverydispatch-kotlin-$PID-$([Guid]::NewGuid().ToString('N'))"
$LogDir = Join-Path $RunDir "logs"
$ConfigDir = Join-Path $RunDir "config"
$StateDir = Join-Path $RunDir "state"
New-Item -ItemType Directory -Force -Path $LogDir, $ConfigDir, $StateDir | Out-Null

$Gradle = if ($IsWindows) { Join-Path $SampleDir "../../gradlew.bat" } else { Join-Path $SampleDir "../../gradlew" }
$Processes = New-Object System.Collections.Generic.List[System.Diagnostics.Process]
$RedisContainer = $null
$LogWaitAttempts = 300
$LogWaitMilliseconds = 100

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
    Remove-Item -Recurse -Force $RunDir -ErrorAction SilentlyContinue
}

function Wait-Port {
    param([string]$Name, [int]$Port)
    for ($attempt = 0; $attempt -lt $LogWaitAttempts; $attempt++) {
        $client = [System.Net.Sockets.TcpClient]::new()
        try {
            $connect = $client.BeginConnect("127.0.0.1", $Port, $null, $null)
            if ($connect.AsyncWaitHandle.WaitOne($LogWaitMilliseconds)) {
                $client.EndConnect($connect)
                return
            }
        } catch {
        } finally {
            $client.Close()
        }
        Start-Sleep -Milliseconds $LogWaitMilliseconds
    }
    throw "Timed out waiting for $Name"
}

function Wait-LogCount {
    param([string[]]$Paths, [string]$Evidence, [int]$Expected)
    for ($attempt = 0; $attempt -lt $LogWaitAttempts; $attempt++) {
        $count = @(
            Select-String -Path $Paths -Pattern $Evidence -SimpleMatch -ErrorAction SilentlyContinue
        ).Count
        if ($count -eq $Expected) {
            return
        }
        if ($count -gt $Expected) {
            throw "Expected $Expected '$Evidence', found $count."
        }
        Start-Sleep -Milliseconds $LogWaitMilliseconds
    }
    throw "Timed out waiting for $Expected '$Evidence'."
}

function Write-Config {
    param([string]$Path, [string]$CourierNode)
    @(
        "trackingChannelEndpoint=tcp://127.0.0.1:$TrackingChannelPort",
        "trackingSpotEndpoint=tcp://127.0.0.1:$TrackingSpotPort",
        "customerStreamEndpoint=tcp://127.0.0.1:$CustomerStreamPort",
        "courierStreamEndpoint=tcp://127.0.0.1:$CourierStreamPort",
        "dispatchHttpEndpoint=http://127.0.0.1:$DispatchHttpPort",
        "dispatchSpotEndpoint=tcp://127.0.0.1:$DispatchSpotPort",
        "dispatchChannelEndpoint=tcp://127.0.0.1:$DispatchChannelPort",
        "customerSpotEndpoint=tcp://127.0.0.1:$CustomerSpotPort",
        "customerSpotRouterEndpoint=tcp://127.0.0.1:$CustomerRouterPort",
        "courierActorNode1SpotEndpoint=tcp://127.0.0.1:$CourierNode1SpotPort",
        "courierActorNode2SpotEndpoint=tcp://127.0.0.1:$CourierNode2SpotPort",
        "courierSessionSpotEndpoint=tcp://127.0.0.1:$CourierSessionSpotPort",
        "redisEndpoint=$RedisEndpoint",
        "redisKeyPrefix=$RedisKeyPrefix",
        "courierNode=$CourierNode",
        "logDirectory=$LogDir",
        "stateDirectory=$StateDir"
    ) | Set-Content -Path $Path -Encoding UTF8
}

function Start-Role {
    param([string]$Project, [string]$ScriptName, [string]$ConfigPath, [string]$LogName)
    $bin = Join-Path $SampleDir "$Project/build/install/$ScriptName/bin/$ScriptName"
    if ($IsWindows) { $bin = "$bin.bat" }
    $process = Start-Process -FilePath $bin -ArgumentList @("--config", $ConfigPath) -WorkingDirectory $SampleDir -NoNewWindow -RedirectStandardOutput (Join-Path $LogDir $LogName) -RedirectStandardError (Join-Path $LogDir "$LogName.err") -PassThru
    $Processes.Add($process)
}

$Status = 1
try {
    $ports = @(Get-ZlinkSampleApplicationPorts -Language Kotlin -Count 12)
    $TrackingChannelPort = $ports[0]
    $TrackingSpotPort = $ports[1]
    $CustomerStreamPort = $ports[2]
    $CourierStreamPort = $ports[3]
    $DispatchHttpPort = $ports[4]
    $DispatchSpotPort = $ports[5]
    $DispatchChannelPort = $ports[6]
    $CustomerSpotPort = $ports[7]
    $CustomerRouterPort = $ports[8]
    $CourierNode1SpotPort = $ports[9]
    $CourierNode2SpotPort = $ports[10]
    $CourierSessionSpotPort = $ports[11]

    $redis = Start-ZlinkSampleRedis "zlink-redis-kotlin-sample-deliverydispatch" "redis:7.2-alpine" -Language Kotlin
    $RedisContainer = $redis.ContainerId
    $RedisEndpoint = $redis.Endpoint
    $redisPort = [int](($RedisEndpoint -replace '^redis://', '').Split(':')[-1])
    Wait-Port "redis" $redisPort
    $RedisKeyPrefix = "deliverydispatch:kotlin:${PID}:$([Guid]::NewGuid().ToString('N')):"

    $trackingConfig = Join-Path $ConfigDir "tracking.properties"
    $customerGatewayConfig = Join-Path $ConfigDir "customer-gateway.properties"
    $courierSessionConfig = Join-Path $ConfigDir "courier-session.properties"
    $courierNode1Config = Join-Path $ConfigDir "courier-node1.properties"
    $courierNode2Config = Join-Path $ConfigDir "courier-node2.properties"
    $dispatchConfig = Join-Path $ConfigDir "dispatch.properties"
    $clientConfig = Join-Path $ConfigDir "client.properties"
    Write-Config $trackingConfig "node1"
    Write-Config $customerGatewayConfig "node1"
    Write-Config $courierSessionConfig "node1"
    Write-Config $courierNode1Config "node1"
    Write-Config $courierNode2Config "node2"
    Write-Config $dispatchConfig "node1"
    Write-Config $clientConfig "node1"

    Push-Location "../../.."
    try {
        Invoke-ZlinkSampleGradleBuild -GradleExecutable $Gradle -Arguments @(
            "--no-daemon", "--no-parallel", "--max-workers=1",
            ":zlink-framework-core:jar",
            ":zlink-framework-spring-boot-starter:jar",
            ":zlink-framework-locations-redis:jar",
            ":zlink-stream-connector:jar",
            "--quiet")
    } finally {
        Pop-Location
    }
    Invoke-ZlinkSampleGradleBuild -GradleExecutable $Gradle -Arguments @(
        "--settings-file", "standalone.settings.gradle.kts", "--no-daemon", "--no-parallel", "--max-workers=1",
        ":Server:Tracking:installDist", ":Server:CustomerGateway:installDist", ":Server:CourierSession:installDist",
        ":Server:CourierSpotNode:installDist", ":Server:Dispatch:installDist", ":Client:installDist", "--quiet")

    Start-Role "Server/Tracking" "Tracking" $trackingConfig "tracking.log"
    Wait-Port "tracking-channel" $TrackingChannelPort
    Wait-Port "tracking-spot" $TrackingSpotPort
    Start-Role "Server/CustomerGateway" "CustomerGateway" $customerGatewayConfig "customer-gateway.log"
    Wait-Port "customer-stream" $CustomerStreamPort
    Wait-Port "customer-router" $CustomerRouterPort
    Start-Role "Server/CourierSession" "CourierSession" $courierSessionConfig "courier-session.log"
    Wait-Port "courier-stream" $CourierStreamPort
    Wait-Port "courier-session-spot" $CourierSessionSpotPort
    Start-Role "Server/CourierSpotNode" "CourierSpotNode" $courierNode1Config "courier-node1.log"
    Start-Role "Server/CourierSpotNode" "CourierSpotNode" $courierNode2Config "courier-node2.log"
    Wait-Port "courier-node-1" $CourierNode1SpotPort
    Wait-Port "courier-node-2" $CourierNode2SpotPort
    Start-Role "Server/Dispatch" "Dispatch" $dispatchConfig "dispatch.log"
    Wait-Port "dispatch-http" $DispatchHttpPort
    Wait-Port "dispatch-spot" $DispatchSpotPort

    Wait-LogCount (Join-Path $LogDir "tracking.log") "deliverydispatch-ready kind=route node=tracking" 1
    Wait-LogCount (Join-Path $LogDir "customer-gateway.log") "deliverydispatch-ready kind=route node=customer-gateway" 1
    Wait-LogCount (Join-Path $LogDir "courier-session.log") "deliverydispatch-ready kind=route node=courier-session" 1
    Wait-LogCount (Join-Path $LogDir "courier-node1.log") "deliverydispatch-ready kind=route node=courier-node-1" 1
    Wait-LogCount (Join-Path $LogDir "courier-node2.log") "deliverydispatch-ready kind=route node=courier-node-2" 1
    Wait-LogCount (Join-Path $LogDir "dispatch.log") "deliverydispatch-ready kind=route node=dispatch" 1
    Wait-LogCount (Join-Path $LogDir "dispatch.log") "deliverydispatch-ready kind=actor-route node=dispatch target=courier-node-1" 1
    Wait-LogCount (Join-Path $LogDir "dispatch.log") "deliverydispatch-ready kind=actor-route node=dispatch target=courier-node-2" 1

    $clientBin = Join-Path $SampleDir "Client/build/install/Client/bin/Client"
    if ($IsWindows) { $clientBin = "$clientBin.bat" }
    $clientLog = Join-Path $LogDir "client.log"
    & $clientBin --config $clientConfig *> $clientLog
    if ($LASTEXITCODE -ne 0) { throw "Client failed." }

    Wait-LogCount $clientLog "deliverydispatch-reassignment=completed" 1
    Wait-LogCount $clientLog "deliverydispatch-server-evidence=completed" 1
    Wait-LogCount $clientLog "deliverydispatch=completed" 1
    Wait-LogCount (Join-Path $LogDir "courier-session.log") "deliverydispatch-courier bound courier=courier-a" 1
    Wait-LogCount (Join-Path $LogDir "courier-session.log") "deliverydispatch-courier bound courier=courier-b" 1
    Wait-LogCount @((Join-Path $LogDir "courier-node1.log"), (Join-Path $LogDir "courier-node2.log")) "deliverydispatch-courier bind-relayed courier=courier-a" 1
    Wait-LogCount @((Join-Path $LogDir "courier-node1.log"), (Join-Path $LogDir "courier-node2.log")) "deliverydispatch-courier bind-relayed courier=courier-b" 1
    Wait-LogCount (Join-Path $LogDir "customer-gateway.log") "deliverydispatch-customer bound customer=customer-1" 1
    Wait-LogCount (Join-Path $LogDir "customer-gateway.log") "deliverydispatch-customer pushed status=Delivered" 2
    Wait-LogCount (Join-Path $LogDir "tracking.log") "deliverydispatch-tracking status=Delivered" 2
    Wait-LogCount (Join-Path $LogDir "dispatch.log") "deliverydispatch-dispatch stale-decision-ignored delivery=delivery-reassign courier=courier-a attempt=1" 1
    Wait-LogCount (Join-Path $LogDir "dispatch.log") "deliverydispatch-dispatch failed delivery=delivery-exhausted reason=candidates-exhausted" 1

    Write-Host "deliverydispatch-placement=completed"
    $Status = 0
} finally {
    Cleanup $Status
}

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
$RunDir = Join-Path ([System.IO.Path]::GetTempPath()) "deliverydispatch-cpp-$PID-$([Guid]::NewGuid().ToString('N'))"
$LogDir = Join-Path $RunDir "logs"
$ConfigDir = Join-Path $RunDir "config"
$FlowLogDir = Join-Path $RunDir "flow-logs"
New-Item -ItemType Directory -Force -Path $LogDir, $ConfigDir, $FlowLogDir | Out-Null

function Find-Binary([string]$Name) {
    foreach ($candidate in @(
        (Join-Path $BuildDir $Name),
        (Join-Path $BuildDir "$Name.exe"),
        (Join-Path $BuildDir "linux-ninja-debug/$Name"),
        (Join-Path $BuildDir "linux-ninja-debug/$Name.exe")
    )) {
        if (Test-Path $candidate) { return $candidate }
    }
    throw "Missing executable: $Name"
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
            if ($pending.AsyncWaitHandle.WaitOne(200)) {
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

function Get-LogLineCount([string[]]$Paths, [string]$ExpectedLine) {
    $count = 0
    foreach ($path in $Paths) {
        if (Test-Path $path) {
            Get-Content -Path $path | ForEach-Object {
                if ($_ -ceq $ExpectedLine) { $count++ }
            }
        }
    }
    return $count
}

function Wait-LogCount([string]$Name, [string[]]$Paths, [string]$ExpectedLine, [int]$ExpectedCount) {
    $actual = 0
    for ($attempt = 0; $attempt -lt $WaitAttempts; $attempt++) {
        $actual = Get-LogLineCount $Paths $ExpectedLine
        if ($actual -eq $ExpectedCount) { return }
        if ($actual -gt $ExpectedCount) { break }
        Start-Sleep -Milliseconds $WaitMilliseconds
    }
    throw "Expected $Name exactly $ExpectedCount time(s), found $actual."
}

function Get-LogPrefixCount([string[]]$Paths, [string]$Prefix) {
    $count = 0
    foreach ($path in $Paths) {
        if (Test-Path $path) {
            Get-Content -Path $path | ForEach-Object {
                if ($_.StartsWith($Prefix, [System.StringComparison]::Ordinal)) { $count++ }
            }
        }
    }
    return $count
}

function Wait-LogPrefixCount([string]$Name, [string[]]$Paths, [string]$Prefix, [int]$ExpectedCount) {
    $actual = 0
    for ($attempt = 0; $attempt -lt $WaitAttempts; $attempt++) {
        $actual = Get-LogPrefixCount $Paths $Prefix
        if ($actual -eq $ExpectedCount) { return }
        if ($actual -gt $ExpectedCount) { break }
        Start-Sleep -Milliseconds $WaitMilliseconds
    }
    throw "Expected $Name exactly $ExpectedCount time(s), found $actual."
}

function Start-Role([string]$Name, [string]$Binary, [string[]]$Arguments) {
    $process = Start-Process -FilePath $Binary -ArgumentList $Arguments -PassThru `
        -RedirectStandardOutput (Join-Path $LogDir "$Name.stdout.log") `
        -RedirectStandardError (Join-Path $LogDir "$Name.stderr.log")
    $Processes.Add($process)
}

function Write-RoleConfig([string]$RoleName, [string]$InstanceName) {
    $role = @{ name = $RoleName; logDir = $FlowLogDir }
    if ($InstanceName) { $role.instanceName = $InstanceName }
    $configuration = @{
        sample = @{
            role = $role
            topology = @{
                redisEndpoint = $RedisEndpoint
                redisKeyPrefix = $RedisKeyPrefix
                dispatchApiHttpUrl = $ApiHttpUrl
                dispatchRouteEndpoint = $DispatchRoute
                dispatchSpotRouterEndpoint = $DispatchSpotRouter
                dispatchSpotEndpoint = $DispatchSpot
                trackingRouteEndpoint = $TrackingRoute
                trackingSpotRouterEndpoint = $TrackingSpotRouter
                trackingSpotEndpoint = $TrackingSpot
                customerStreamEndpoint = $CustomerStream
                customerSpotRouterEndpoint = $CustomerSpotRouter
                customerSpotEndpoint = $CustomerSpot
                courierStreamEndpoint = $CourierStream
                courierSessionSpotRouterEndpoint = $CourierSessionSpotRouter
                courierSessionSpotEndpoint = $CourierSessionSpot
                courierActorNode1RouteEndpoint = $CourierNode1Route
                courierActorNode1RouterEndpoint = $CourierNode1Router
                courierActorNode1Endpoint = $CourierNode1
                courierActorNode2RouteEndpoint = $CourierNode2Route
                courierActorNode2RouterEndpoint = $CourierNode2Router
                courierActorNode2Endpoint = $CourierNode2
            }
        }
    }
    $configuration | ConvertTo-Json -Depth 5 | Set-Content -Path (Join-Path $ConfigDir "$RoleName.json") -Encoding utf8
}

function Cleanup([int]$Status) {
    foreach ($process in $Processes) {
        try {
            if (-not $process.HasExited) { Stop-Process -Id $process.Id -ErrorAction SilentlyContinue }
            if (-not $process.WaitForExit(1000)) { $Status = 1 }
        } catch { $Status = 1 }
    }
    if ($RedisContainer) { Remove-ZlinkSampleRedis $RedisContainer }
    if (Test-Path $RunDir) { Remove-Item -Recurse -Force $RunDir }
    return $Status
}

$Status = 1
try {
    & cmake --build $BuildDir --parallel 2 --target `
        sample_cpp_framework_deliverydispatch_dispatch `
        sample_cpp_framework_deliverydispatch_courier_actor_node `
        sample_cpp_framework_deliverydispatch_customer_gateway `
        sample_cpp_framework_deliverydispatch_courier_session `
        sample_cpp_framework_deliverydispatch_tracking `
        sample_cpp_framework_deliverydispatch_client
    if ($LASTEXITCODE -ne 0) { throw "DeliveryDispatch sample build failed." }

    $ports = @(Get-ZlinkSamplePorts -Count 20 -Paired)
    $ApiHttpUrl = "http://127.0.0.1:$($ports[1])"
    $DispatchRoute = "tcp://127.0.0.1:$($ports[2])"
    $DispatchSpotRouter = "tcp://127.0.0.1:$($ports[3])"
    $TrackingRoute = "tcp://127.0.0.1:$($ports[4])"
    $TrackingSpotRouter = "tcp://127.0.0.1:$($ports[5])"
    $TrackingSpot = "tcp://127.0.0.1:$($ports[6])"
    $DispatchSpot = "tcp://127.0.0.1:$($ports[7])"
    $CustomerStream = "tcp://127.0.0.1:$($ports[8])"
    $CustomerSpotRouter = "tcp://127.0.0.1:$($ports[9])"
    $CustomerSpot = "tcp://127.0.0.1:$($ports[10])"
    $CourierStream = "tcp://127.0.0.1:$($ports[11])"
    $CourierSessionSpotRouter = "tcp://127.0.0.1:$($ports[12])"
    $CourierSessionSpot = "tcp://127.0.0.1:$($ports[13])"
    $CourierNode1Route = "tcp://127.0.0.1:$($ports[14])"
    $CourierNode1Router = "tcp://127.0.0.1:$($ports[15])"
    $CourierNode1 = "tcp://127.0.0.1:$($ports[16])"
    $CourierNode2Route = "tcp://127.0.0.1:$($ports[17])"
    $CourierNode2Router = "tcp://127.0.0.1:$($ports[18])"
    $CourierNode2 = "tcp://127.0.0.1:$($ports[19])"

    $redis = Start-ZlinkSampleRedis "zlink-redis-cpp-sample-deliverydispatch"
    $RedisContainer = $redis.ContainerId
    $RedisEndpoint = "tcp://$($redis.Endpoint)"
    $RedisKeyPrefix = "deliverydispatch:$PID:$([Guid]::NewGuid().ToString('N')):"
    Wait-Endpoint "redis" $RedisEndpoint

    Write-RoleConfig "tracking" ""
    Write-RoleConfig "customer-gateway" ""
    Write-RoleConfig "courier-session" ""
    Write-RoleConfig "dispatch" ""
    Write-RoleConfig "courier-node-1" "courier-node-1"
    Write-RoleConfig "courier-node-2" "courier-node-2"

    $DispatchBin = Find-Binary "sample_cpp_framework_deliverydispatch_dispatch"
    $CourierNodeBin = Find-Binary "sample_cpp_framework_deliverydispatch_courier_actor_node"
    $CustomerGatewayBin = Find-Binary "sample_cpp_framework_deliverydispatch_customer_gateway"
    $CourierSessionBin = Find-Binary "sample_cpp_framework_deliverydispatch_courier_session"
    $TrackingBin = Find-Binary "sample_cpp_framework_deliverydispatch_tracking"
    $ClientBin = Find-Binary "sample_cpp_framework_deliverydispatch_client"
    Start-Role "tracking" $TrackingBin @("--config=$(Join-Path $ConfigDir 'tracking.json')")
    Start-Role "customer-gateway" $CustomerGatewayBin @("--config=$(Join-Path $ConfigDir 'customer-gateway.json')")
    Start-Role "courier-session" $CourierSessionBin @("--config=$(Join-Path $ConfigDir 'courier-session.json')")
    Start-Role "courier-node-1" $CourierNodeBin @("--config=$(Join-Path $ConfigDir 'courier-node-1.json')")
    Start-Role "courier-node-2" $CourierNodeBin @("--config=$(Join-Path $ConfigDir 'courier-node-2.json')")
    Start-Role "dispatch" $DispatchBin @("--config=$(Join-Path $ConfigDir 'dispatch.json')")

    Wait-Endpoint "tracking" $TrackingRoute
    Wait-Endpoint "tracking spot" $TrackingSpotRouter
    Wait-Endpoint "customer stream" $CustomerStream
    Wait-Endpoint "customer spot" $CustomerSpotRouter
    Wait-Endpoint "courier stream" $CourierStream
    Wait-Endpoint "courier session spot" $CourierSessionSpotRouter
    Wait-Endpoint "courier node 1 spot" $CourierNode1Router
    Wait-Endpoint "courier node 2 spot" $CourierNode2Router
    Wait-Endpoint "dispatch" $DispatchRoute
    Wait-Endpoint "dispatch HTTP" $ApiHttpUrl

    $trackingLogs = @((Join-Path $LogDir "tracking.stdout.log"), (Join-Path $LogDir "tracking.stderr.log"))
    $customerLogs = @((Join-Path $LogDir "customer-gateway.stdout.log"), (Join-Path $LogDir "customer-gateway.stderr.log"))
    $courierSessionLogs = @((Join-Path $LogDir "courier-session.stdout.log"), (Join-Path $LogDir "courier-session.stderr.log"))
    $courierNodeLogs = @((Join-Path $LogDir "courier-node-1.stdout.log"), (Join-Path $LogDir "courier-node-1.stderr.log"), (Join-Path $LogDir "courier-node-2.stdout.log"), (Join-Path $LogDir "courier-node-2.stderr.log"))
    $dispatchLogs = @((Join-Path $LogDir "dispatch.stdout.log"), (Join-Path $LogDir "dispatch.stderr.log"))
    Wait-LogCount "tracking route readiness" $trackingLogs "deliverydispatch-ready kind=route node=tracking" 1
    Wait-LogCount "customer gateway route readiness" $customerLogs "deliverydispatch-ready kind=route node=customer-gateway" 1
    Wait-LogCount "courier session route readiness" $courierSessionLogs "deliverydispatch-ready kind=route node=courier-session" 1
    Wait-LogCount "courier node 1 route readiness" $courierNodeLogs "deliverydispatch-ready kind=route node=courier-node-1" 1
    Wait-LogCount "courier node 2 route readiness" $courierNodeLogs "deliverydispatch-ready kind=route node=courier-node-2" 1
    Wait-LogCount "dispatch route readiness" $dispatchLogs "deliverydispatch-ready kind=route node=dispatch" 1
    Wait-LogCount "dispatch actor route courier node 1" $dispatchLogs "deliverydispatch-ready kind=actor-route node=dispatch target=courier-node-1" 1
    Wait-LogCount "dispatch actor route courier node 2" $dispatchLogs "deliverydispatch-ready kind=actor-route node=dispatch target=courier-node-2" 1

    $clientLog = Join-Path $LogDir "client.log"
    & $ClientBin --api-url $ApiHttpUrl --stream-endpoint $CustomerStream --courier-stream-endpoint $CourierStream *> $clientLog
    if ($LASTEXITCODE -ne 0) { throw "DeliveryDispatch client failed." }
    Wait-LogCount "client server evidence completion marker" @($clientLog) "deliverydispatch-server-evidence=completed" 1
    Wait-LogCount "client reassignment completion marker" @($clientLog) "deliverydispatch-reassignment=completed" 1
    Wait-LogCount "client completion marker" @($clientLog) "deliverydispatch=completed" 1
    Wait-LogCount "courier a bind" $courierSessionLogs "deliverydispatch-courier bound courier=courier-a" 1
    Wait-LogCount "courier b bind" $courierSessionLogs "deliverydispatch-courier bound courier=courier-b" 1
    Wait-LogCount "courier a bind relay" $courierNodeLogs "deliverydispatch-courier bind-relayed courier=courier-a" 1
    Wait-LogCount "courier b bind relay" $courierNodeLogs "deliverydispatch-courier bind-relayed courier=courier-b" 1
    Wait-LogCount "customer bind" $customerLogs "deliverydispatch-customer bound customer=customer-1" 1
    Wait-LogCount "success delivered customer push" $customerLogs "deliverydispatch-customer pushed status=Delivered delivery=delivery-success" 1
    Wait-LogCount "reassigned delivered customer push" $customerLogs "deliverydispatch-customer pushed status=Delivered delivery=delivery-reassign" 1
    Wait-LogPrefixCount "all delivered customer pushes" $customerLogs "deliverydispatch-customer pushed status=Delivered delivery=" 2
    Wait-LogCount "success delivered tracking status" $trackingLogs "deliverydispatch-tracking status=Delivered delivery=delivery-success" 1
    Wait-LogCount "reassigned delivered tracking status" $trackingLogs "deliverydispatch-tracking status=Delivered delivery=delivery-reassign" 1
    Wait-LogPrefixCount "all delivered tracking status" $trackingLogs "deliverydispatch-tracking status=Delivered delivery=" 2
    Wait-LogCount "stale courier decision" $dispatchLogs "deliverydispatch-dispatch stale-decision-ignored delivery=delivery-reassign courier=courier-a attempt=1" 1
    Wait-LogCount "candidates exhausted" $dispatchLogs "deliverydispatch-dispatch failed delivery=delivery-exhausted reason=candidates-exhausted" 1
    $Status = 0
} finally {
    $Status = Cleanup $Status
}

if ($Status -ne 0) { exit $Status }
Write-Host "deliverydispatch-placement=completed"

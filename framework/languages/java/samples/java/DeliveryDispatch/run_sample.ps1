Set-StrictMode -Version Latest
. "$PSScriptRoot/../../redis-common.ps1"
$ErrorActionPreference = "Stop"

$SampleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $SampleDir
$WaitAttempts = 300
$WaitIntervalMilliseconds = 100
$LogDir = Join-Path $SampleDir "build/sample-logs"
$FlowLogDir = Join-Path $SampleDir "logs"
$ConfigDir = Join-Path ([IO.Path]::GetTempPath()) ("zlink-deliverydispatch-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $LogDir, $FlowLogDir, $ConfigDir | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $LogDir "*.log")
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $FlowLogDir "*.log")

$Gradle = if ($IsWindows) { Join-Path $SampleDir "../../gradlew.bat" } else { Join-Path $SampleDir "../../gradlew" }
$Processes = New-Object System.Collections.Generic.List[System.Diagnostics.Process]
$RedisContainer = $null
$CleanedUp = $false

function Get-ChildProcessIds {
    param([int]$ParentId)
    if ($IsWindows) {
        Get-CimInstance Win32_Process -Filter "ParentProcessId=$ParentId" | ForEach-Object {
            [int]$_.ProcessId
            Get-ChildProcessIds -ParentId ([int]$_.ProcessId)
        }
    } else {
        & pgrep -P $ParentId 2>$null | ForEach-Object {
            if ($_ -match '^\d+$') {
                [int]$_
                Get-ChildProcessIds -ParentId ([int]$_)
            }
        }
    }
}

function Stop-TrackedProcessTree {
    param([System.Diagnostics.Process]$Process)
    $children = @(Get-ChildProcessIds -ParentId $Process.Id)
    [array]::Reverse($children)
    foreach ($childId in $children) { Stop-Process -Id $childId -Force -ErrorAction SilentlyContinue }
    if (-not $Process.HasExited) { Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue }
}

function Cleanup {
    for ($i = $Processes.Count - 1; $i -ge 0; $i--) { Stop-TrackedProcessTree -Process $Processes[$i] }
    if ($RedisContainer) { Remove-ZlinkSampleRedis $RedisContainer }
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $ConfigDir
}

function Split-Endpoint {
    param([string]$Endpoint)
    $parts = $Endpoint.Split(":")
    return @{ Host = $parts[0]; Port = [int]$parts[1] }
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

function Get-AppBin {
    param([string]$Project, [string]$Name)
    $binDir = Join-Path $SampleDir "$Project/build/install/$Name/bin"
    return Join-Path $binDir $(if ($IsWindows) { "$Name.bat" } else { $Name })
}

function Start-AppRole {
    param([string]$Project, [string]$Name, [string]$Config, [string]$LogName)
    $process = Start-Process -FilePath (Get-AppBin $Project $Name) -ArgumentList @("--config", $Config) `
        -WorkingDirectory $SampleDir -NoNewWindow -RedirectStandardOutput (Join-Path $LogDir $LogName) `
        -RedirectStandardError (Join-Path $LogDir ($LogName + ".err.log")) -PassThru
    $Processes.Add($process)
}

function Get-LogCount {
    param([string[]]$Paths, [string]$Evidence)
    $count = 0
    foreach ($path in $Paths) {
        if (Test-Path $path) {
            $count += @(Get-Content -Path $path -ErrorAction SilentlyContinue | Where-Object {
                $_.IndexOf($Evidence, [System.StringComparison]::Ordinal) -ge 0
            }).Count
        }
    }
    return $count
}

function Wait-LogCount {
    param([string]$Description, [string[]]$Paths, [string]$Evidence, [int]$Expected)
    for ($attempt = 0; $attempt -lt $WaitAttempts; $attempt++) {
        $actual = Get-LogCount -Paths $Paths -Evidence $Evidence
        if ($actual -eq $Expected) { return }
        if ($actual -gt $Expected) { throw "DeliveryDispatch ${Description}: expected $Expected '$Evidence', found $actual." }
        Start-Sleep -Milliseconds $WaitIntervalMilliseconds
    }
    throw "DeliveryDispatch ${Description}: timed out waiting for $Expected '$Evidence'."
}

$Status = 1
try {
    $endpoints = @(Get-ZlinkSampleApplicationEndpoints -Language Java -Count 13)
    $tracking = Split-Endpoint $endpoints[0]
    $customerStream = Split-Endpoint $endpoints[1]
    $courierStream = Split-Endpoint $endpoints[2]
    $dispatchHttp = Split-Endpoint $endpoints[3]
    $dispatchSpot = Split-Endpoint $endpoints[4]
    $dispatchChannel = Split-Endpoint $endpoints[5]
    $customerSpot = Split-Endpoint $endpoints[6]
    $customerRouter = Split-Endpoint $endpoints[7]
    $trackingSpotRouter = Split-Endpoint $endpoints[8]
    $trackingSpotPub = Split-Endpoint $endpoints[9]
    $courierNode1Spot = Split-Endpoint $endpoints[10]
    $courierNode2Spot = Split-Endpoint $endpoints[11]
    $courierSessionSpot = Split-Endpoint $endpoints[12]

    $redis = Start-ZlinkSampleRedis "zlink-redis-java-sample-deliverydispatch" -Language Java
    $RedisContainer = $redis.ContainerId
    $redisEndpoint = $redis.Endpoint
    $redisKeyPrefix = "deliverydispatch:java:${PID}:$([Guid]::NewGuid().ToString('N')):"

    function Write-SampleConfig {
        param([string]$Name, [string]$Role, [string]$CourierNode = "node1")
        $path = Join-Path $ConfigDir "$Name.properties"
        if ($Role -eq "client") {
            Set-Content -Path $path -Value @(
                "customerStreamEndpoint=tcp://$($customerStream.Host):$($customerStream.Port)",
                "courierStreamEndpoint=tcp://$($courierStream.Host):$($courierStream.Port)",
                "dispatchHttpEndpoint=http://$($dispatchHttp.Host):$($dispatchHttp.Port)") -Encoding utf8NoBOM
            Protect-ConfigFile $path
            return $path
        }
        $lines = [System.Collections.Generic.List[string]]::new()
        $lines.Add("sample.redisEndpoint=$redisEndpoint")
        $lines.Add("sample.redisKeyPrefix=$redisKeyPrefix")
        $lines.Add("sample.logDirectory=$($FlowLogDir.Replace('\', '/'))")
        switch ($Role) {
            "tracking" { $lines.AddRange(@("sample.trackingChannelEndpoint=tcp://$($tracking.Host):$($tracking.Port)", "sample.trackingSpotEndpoint=tcp://$($trackingSpotRouter.Host):$($trackingSpotRouter.Port)", "sample.trackingSpotPubEndpoint=tcp://$($trackingSpotPub.Host):$($trackingSpotPub.Port)")) }
            "customer-gateway" { $lines.AddRange(@("sample.customerStreamEndpoint=tcp://$($customerStream.Host):$($customerStream.Port)", "sample.customerSpotEndpoint=tcp://$($customerSpot.Host):$($customerSpot.Port)", "sample.customerSpotRouterEndpoint=tcp://$($customerRouter.Host):$($customerRouter.Port)")) }
            "courier-session" { $lines.AddRange(@("sample.courierStreamEndpoint=tcp://$($courierStream.Host):$($courierStream.Port)", "sample.courierSessionSpotEndpoint=tcp://$($courierSessionSpot.Host):$($courierSessionSpot.Port)")) }
            "courier-node" { $lines.Add("sample.courierNode=$CourierNode"); if ($CourierNode -eq "node2") { $lines.Add("sample.courierActorNode2SpotEndpoint=tcp://$($courierNode2Spot.Host):$($courierNode2Spot.Port)") } else { $lines.Add("sample.courierActorNode1SpotEndpoint=tcp://$($courierNode1Spot.Host):$($courierNode1Spot.Port)") } }
            "dispatch" { $lines.AddRange(@("sample.dispatchHttpEndpoint=http://$($dispatchHttp.Host):$($dispatchHttp.Port)", "sample.dispatchSpotEndpoint=tcp://$($dispatchSpot.Host):$($dispatchSpot.Port)", "sample.dispatchChannelEndpoint=tcp://$($dispatchChannel.Host):$($dispatchChannel.Port)")) }
        }
        Set-Content -Path $path -Value $lines -Encoding utf8NoBOM
        Protect-ConfigFile $path
        return $path
    }

    $trackingConfig = Write-SampleConfig "tracking" "tracking"
    $customerGatewayConfig = Write-SampleConfig "customer-gateway" "customer-gateway"
    $courierSessionConfig = Write-SampleConfig "courier-session" "courier-session"
    $courierNode1Config = Write-SampleConfig "courier-node-1" "courier-node" "node1"
    $courierNode2Config = Write-SampleConfig "courier-node-2" "courier-node" "node2"
    $dispatchConfig = Write-SampleConfig "dispatch" "dispatch"
    $clientConfig = Write-SampleConfig "client" "client"

    Push-Location "../../.."
    try {
        Invoke-ZlinkSampleGradleBuild -GradleExecutable $Gradle -Arguments @("--no-daemon", ":zlink-framework-core:jar", ":zlink-framework-spring-boot-starter:jar", ":zlink-framework-locations-redis:jar", ":zlink-stream-connector:jar", "--quiet")
    } finally { Pop-Location }
    Invoke-ZlinkSampleGradleBuild -GradleExecutable $Gradle -Arguments @("--settings-file", "standalone.settings.gradle.kts", "--no-daemon", ":Server:Tracking:installDist", ":Server:CustomerGateway:installDist", ":Server:CourierSession:installDist", ":Server:CourierSpotNode:installDist", ":Server:Dispatch:installDist", ":Client:installDist", "--quiet")

    Start-AppRole "Server/Tracking" "Tracking" $trackingConfig "tracking.log"
    Start-AppRole "Server/CustomerGateway" "CustomerGateway" $customerGatewayConfig "customer-gateway.log"
    Start-AppRole "Server/CourierSession" "CourierSession" $courierSessionConfig "courier-session.log"
    Start-AppRole "Server/CourierSpotNode" "CourierSpotNode" $courierNode1Config "courier-node-1.log"
    Start-AppRole "Server/CourierSpotNode" "CourierSpotNode" $courierNode2Config "courier-node-2.log"
    Start-AppRole "Server/Dispatch" "Dispatch" $dispatchConfig "dispatch.log"

    $trackingLog = Join-Path $LogDir "tracking.log"
    $customerLog = Join-Path $LogDir "customer-gateway.log"
    $courierSessionLog = Join-Path $LogDir "courier-session.log"
    $courierNodeLogs = @((Join-Path $LogDir "courier-node-1.log"), (Join-Path $LogDir "courier-node-2.log"))
    $dispatchLog = Join-Path $LogDir "dispatch.log"
    Wait-LogCount "tracking route readiness" @($trackingLog) "deliverydispatch-ready kind=route node=tracking" 1
    Wait-LogCount "customer gateway route readiness" @($customerLog) "deliverydispatch-ready kind=route node=customer-gateway" 1
    Wait-LogCount "courier session route readiness" @($courierSessionLog) "deliverydispatch-ready kind=route node=courier-session" 1
    Wait-LogCount "courier node 1 route readiness" @($courierNodeLogs[0]) "deliverydispatch-ready kind=route node=courier-node-1" 1
    Wait-LogCount "courier node 2 route readiness" @($courierNodeLogs[1]) "deliverydispatch-ready kind=route node=courier-node-2" 1
    Wait-LogCount "dispatch route readiness" @($dispatchLog) "deliverydispatch-ready kind=route node=dispatch" 1
    Wait-LogCount "dispatch courier node 1 readiness" @($dispatchLog) "deliverydispatch-ready kind=actor-route node=dispatch target=courier-node-1" 1
    Wait-LogCount "dispatch courier node 2 readiness" @($dispatchLog) "deliverydispatch-ready kind=actor-route node=dispatch target=courier-node-2" 1

    $clientLog = Join-Path $LogDir "client.log"
    & (Get-AppBin "Client" "Client") --config $clientConfig *> $clientLog
    if ($LASTEXITCODE -ne 0) { throw "Client run failed." }
    Wait-LogCount "client reassignment marker" @($clientLog) "deliverydispatch-reassignment=completed" 1
    Wait-LogCount "client server evidence marker" @($clientLog) "deliverydispatch-server-evidence=completed" 1
    Wait-LogCount "client completion marker" @($clientLog) "deliverydispatch=completed" 1
    Wait-LogCount "courier a binding" @($courierSessionLog) "deliverydispatch-courier bound courier=courier-a" 1
    Wait-LogCount "courier b binding" @($courierSessionLog) "deliverydispatch-courier bound courier=courier-b" 1
    Wait-LogCount "courier a bind relay" $courierNodeLogs "deliverydispatch-courier bind-relayed courier=courier-a" 1
    Wait-LogCount "courier b bind relay" $courierNodeLogs "deliverydispatch-courier bind-relayed courier=courier-b" 1
    Wait-LogCount "customer binding" @($customerLog) "deliverydispatch-customer bound customer=customer-1" 1
    Wait-LogCount "customer delivered pushes" @($customerLog) "deliverydispatch-customer pushed status=Delivered delivery=" 2
    Wait-LogCount "tracking delivered statuses" @($trackingLog) "deliverydispatch-tracking status=Delivered delivery=" 2
    Wait-LogCount "stale decision" @($dispatchLog) "deliverydispatch-dispatch stale-decision-ignored delivery=delivery-reassign courier=courier-a attempt=1" 1
    Wait-LogCount "candidates exhausted" @($dispatchLog) "deliverydispatch-dispatch failed delivery=delivery-exhausted reason=candidates-exhausted" 1

    Cleanup | Out-Null
    $CleanedUp = $true
    $Status = 0
    Write-Output "deliverydispatch-placement=completed"
} finally {
    if (-not $CleanedUp) { Cleanup | Out-Null }
}

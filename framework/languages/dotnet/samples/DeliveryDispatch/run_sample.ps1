$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptDir "../sample_runner.ps1")

$RunDir = New-SampleRunDirectory "deliverydispatch-dotnet"
$RedisContainer = $null
$RunSucceeded = $false
$LogDir = Join-Path $RunDir "logs"
$WorkDir = Join-Path $RunDir "work"
$SampleLogDir = Join-Path $RunDir "sample-logs"
$ConfigDir = Join-Path $RunDir "config"
$WaitAttempts = 300
$WaitIntervalMilliseconds = 100
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
New-Item -ItemType Directory -Force -Path $SampleLogDir | Out-Null
New-Item -ItemType Directory -Force -Path $ConfigDir | Out-Null

function Wait-DeliveryDispatchTcpEndpoint {
    param([string]$Name, [string]$Endpoint)

    $uri = [Uri]$Endpoint
    for ($attempt = 1; $attempt -le $WaitAttempts; $attempt++) {
        $client = [System.Net.Sockets.TcpClient]::new()
        try {
            $connect = $client.ConnectAsync($uri.Host, $uri.Port)
            if ($connect.Wait($WaitIntervalMilliseconds) -and $client.Connected) {
                return
            }
        }
        finally {
            $client.Dispose()
        }
        Start-Sleep -Milliseconds $WaitIntervalMilliseconds
    }
    throw "Timed out waiting for $Name at $Endpoint."
}

try {
    $basePort = if ($DELIVERYDISPATCH_BASE_PORT) { [int]$DELIVERYDISPATCH_BASE_PORT } else { 0 }
    $ports = New-SamplePorts -Count 9 -BasePort $basePort

    $RedisKeyPrefix = "deliverydispatch:dotnet:${PID}:$([Guid]::NewGuid().ToString('N')):"
    $DispatchHttp = "http://127.0.0.1:$($ports[0])"
    $DispatchMesh = "tcp://127.0.0.1:$($ports[1])"
    $TrackingMesh = "tcp://127.0.0.1:$($ports[2])"
    $CustomerStream = "tcp://127.0.0.1:$($ports[3])"
    $CustomerMesh = "tcp://127.0.0.1:$($ports[4])"
    $CourierStream = "tcp://127.0.0.1:$($ports[5])"
    $CourierSessionMesh = "tcp://127.0.0.1:$($ports[6])"
    $CourierNode1Mesh = "tcp://127.0.0.1:$($ports[7])"
    $CourierNode2Mesh = "tcp://127.0.0.1:$($ports[8])"

    $redis = Start-SampleRedisContainer "zlink-deliverydispatch-dotnet-redis"
    $RedisContainer = $redis.ContainerId
    $RedisEndpoint = $redis.Endpoint
    Wait-DeliveryDispatchTcpEndpoint "redis" "tcp://$RedisEndpoint"

    # Each role gets one configuration file. The runner picks this run's ports, but it hands them
    # over in a file rather than through the environment
    # (framework/doc/framework/common/sample-e2e-configuration-policy.ko.md 2.2, 7).
    function Write-RoleConfig {
        param([string]$Role)

        $meshEndpoint = switch ($Role) {
            "dispatch" { $DispatchMesh }
            "tracking" { $TrackingMesh }
            "customer-gateway" { $CustomerMesh }
            "courier-session" { $CourierSessionMesh }
            "courier-node-1" { $CourierNode1Mesh }
            "courier-node-2" { $CourierNode2Mesh }
            "client" { "unused" }
        }

        python3 (Join-Path $ScriptDir "write_role_config.py") `
            --output (Join-Path $ConfigDir "$Role.json") `
            --role $Role `
            --log-dir $SampleLogDir `
            --work-dir $WorkDir `
            --redis-endpoint $RedisEndpoint `
            --redis-key-prefix $RedisKeyPrefix `
            --dispatch-http $DispatchHttp `
            --mesh-endpoint $meshEndpoint `
            --customer-stream $CustomerStream `
            --courier-stream $CourierStream
        if ($LASTEXITCODE -ne 0) { throw "Could not write the $Role configuration file." }
    }

    Write-RoleConfig "tracking"
    Write-RoleConfig "customer-gateway"
    Write-RoleConfig "courier-session"
    Write-RoleConfig "dispatch"
    Write-RoleConfig "courier-node-1"
    Write-RoleConfig "courier-node-2"
    Write-RoleConfig "client"

    Invoke-SampleDotnetBuild (Join-Path $ScriptDir "DeliveryDispatch.sln")

    Start-SampleDotnetAssembly -Name "tracking" -Project (Join-Path $ScriptDir "Server/Tracking/DeliveryDispatch.Server.Tracking.csproj") -LogDirectory $LogDir -Arguments @("--config", (Join-Path $ConfigDir "tracking.json")) | Out-Null
    Start-SampleDotnetAssembly -Name "customer-gateway" -Project (Join-Path $ScriptDir "Server/CustomerGateway/DeliveryDispatch.Server.CustomerGateway.csproj") -LogDirectory $LogDir -Arguments @("--config", (Join-Path $ConfigDir "customer-gateway.json")) | Out-Null
    Start-SampleDotnetAssembly -Name "courier-node-1" -Project (Join-Path $ScriptDir "Server/CourierActorNode/DeliveryDispatch.Server.CourierActorNode.csproj") -LogDirectory $LogDir -Arguments @("--config", (Join-Path $ConfigDir "courier-node-1.json")) | Out-Null
    Start-SampleDotnetAssembly -Name "courier-node-2" -Project (Join-Path $ScriptDir "Server/CourierActorNode/DeliveryDispatch.Server.CourierActorNode.csproj") -LogDirectory $LogDir -Arguments @("--config", (Join-Path $ConfigDir "courier-node-2.json")) | Out-Null
    Start-SampleDotnetAssembly -Name "courier-session" -Project (Join-Path $ScriptDir "Server/CourierSession/DeliveryDispatch.Server.CourierSession.csproj") -LogDirectory $LogDir -Arguments @("--config", (Join-Path $ConfigDir "courier-session.json")) | Out-Null
    Start-SampleDotnetAssembly -Name "dispatch" -Project (Join-Path $ScriptDir "Server/Dispatch/DeliveryDispatch.Server.Dispatch.csproj") -LogDirectory $LogDir -Arguments @("--config", (Join-Path $ConfigDir "dispatch.json")) | Out-Null

    function Wait-DeliveryDispatchLogCount {
        param([string]$Description, [string]$Path, [string]$Line, [int]$Expected)

        for ($attempt = 1; $attempt -le $WaitAttempts; $attempt++) {
            $actual = if (Test-Path -LiteralPath $Path) {
                @(Get-Content -LiteralPath $Path | Where-Object { $_.Contains($Line) }).Count
            } else {
                0
            }
            if ($actual -eq $Expected) { return }
            Start-Sleep -Milliseconds $WaitIntervalMilliseconds
        }
        throw "Timed out waiting for ${Description}: expected $Expected occurrence(s) of '$Line' in $Path."
    }

    Wait-DeliveryDispatchLogCount "tracking route readiness" (Join-Path $LogDir "tracking.log") "deliverydispatch-ready kind=route node=tracking" 1
    Wait-DeliveryDispatchLogCount "customer gateway route readiness" (Join-Path $LogDir "customer-gateway.log") "deliverydispatch-ready kind=route node=customer-gateway" 1
    Wait-DeliveryDispatchLogCount "courier session route readiness" (Join-Path $LogDir "courier-session.log") "deliverydispatch-ready kind=route node=courier-session" 1
    Wait-DeliveryDispatchLogCount "courier node 1 route readiness" (Join-Path $LogDir "courier-node-1.log") "deliverydispatch-ready kind=route node=courier-node-1" 1
    Wait-DeliveryDispatchLogCount "courier node 2 route readiness" (Join-Path $LogDir "courier-node-2.log") "deliverydispatch-ready kind=route node=courier-node-2" 1
    Wait-DeliveryDispatchLogCount "dispatch route readiness" (Join-Path $LogDir "dispatch.log") "deliverydispatch-ready kind=route node=dispatch" 1
    Wait-DeliveryDispatchLogCount "dispatch courier node 1 readiness" (Join-Path $LogDir "dispatch.log") "deliverydispatch-ready kind=actor-route node=dispatch target=courier-node-1" 1
    Wait-DeliveryDispatchLogCount "dispatch courier node 2 readiness" (Join-Path $LogDir "dispatch.log") "deliverydispatch-ready kind=actor-route node=dispatch target=courier-node-2" 1

    $clientLog = Join-Path $LogDir "client.log"
    Invoke-SampleDotnetRun -Project (Join-Path $ScriptDir "Client/DeliveryDispatch.Client.csproj") -Arguments @("--config", (Join-Path $ConfigDir "client.json")) *> $clientLog
    Wait-DeliveryDispatchLogCount "client completion" $clientLog "deliverydispatch=completed" 1
    Wait-DeliveryDispatchLogCount "client reassignment completion" $clientLog "deliverydispatch-reassignment=completed" 1
    Wait-DeliveryDispatchLogCount "client server-evidence completion" $clientLog "deliverydispatch-server-evidence=completed" 1
    Wait-DeliveryDispatchLogCount "courier a bound" (Join-Path $LogDir "courier-session.log") "deliverydispatch-courier bound courier=courier-a" 1
    Wait-DeliveryDispatchLogCount "courier b bound" (Join-Path $LogDir "courier-session.log") "deliverydispatch-courier bound courier=courier-b" 1
    Wait-DeliveryDispatchLogCount "courier a bind relayed" (Join-Path $LogDir "courier-session.log") "deliverydispatch-courier bind-relayed courier=courier-a" 1
    Wait-DeliveryDispatchLogCount "courier b bind relayed" (Join-Path $LogDir "courier-session.log") "deliverydispatch-courier bind-relayed courier=courier-b" 1
    Wait-DeliveryDispatchLogCount "customer bound" (Join-Path $LogDir "customer-gateway.log") "deliverydispatch-customer bound customer=customer-1" 1
    Wait-DeliveryDispatchLogCount "customer delivered pushes" (Join-Path $LogDir "customer-gateway.log") "deliverydispatch-customer pushed status=Delivered" 2
    Wait-DeliveryDispatchLogCount "tracking delivered statuses" (Join-Path $LogDir "tracking.log") "deliverydispatch-tracking status=Delivered" 2
    Wait-DeliveryDispatchLogCount "stale courier decision" (Join-Path $LogDir "dispatch.log") "deliverydispatch-dispatch stale-decision-ignored delivery=delivery-reassign courier=courier-a attempt=1" 1
    Wait-DeliveryDispatchLogCount "candidates exhausted" (Join-Path $LogDir "dispatch.log") "deliverydispatch-dispatch failed delivery=delivery-exhausted reason=candidates-exhausted" 1
    Write-Host "deliverydispatch-placement=completed"
    $RunSucceeded = $true
}
finally {
    Remove-SampleConfigurationFiles -RunDirectory $RunDir
    Stop-SampleProcesses
    if ($RedisContainer) {
        Remove-SampleRedisContainer $RedisContainer
    }
    if (-not $RunSucceeded -or $env:DELIVERYDISPATCH_KEEP_RUN_DIR -eq "1") {
        Write-Host "runDir=$RunDir"
    }
    else {
        Remove-Item -Recurse -Force $RunDir -ErrorAction SilentlyContinue
    }
}

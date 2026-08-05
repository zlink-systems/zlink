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
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
New-Item -ItemType Directory -Force -Path $SampleLogDir | Out-Null
New-Item -ItemType Directory -Force -Path $ConfigDir | Out-Null

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
    Wait-SampleTcpEndpoint "redis" "tcp://$RedisEndpoint"

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
            "courier-actor-node1" { $CourierNode1Mesh }
            "courier-actor-node2" { $CourierNode2Mesh }
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
    Write-RoleConfig "courier-actor-node1"
    Write-RoleConfig "courier-actor-node2"
    Write-RoleConfig "client"

    Invoke-SampleDotnetBuild (Join-Path $ScriptDir "DeliveryDispatch.sln")

    Start-SampleDotnetAssembly -Name "tracking" -Project (Join-Path $ScriptDir "Server/Tracking/DeliveryDispatch.Server.Tracking.csproj") -LogDirectory $LogDir -Arguments @("--config", (Join-Path $ConfigDir "tracking.json")) | Out-Null
    Wait-SampleTcpEndpoint "tracking-mesh" $TrackingMesh

    Start-SampleDotnetAssembly -Name "customer-gateway" -Project (Join-Path $ScriptDir "Server/CustomerGateway/DeliveryDispatch.Server.CustomerGateway.csproj") -LogDirectory $LogDir -Arguments @("--config", (Join-Path $ConfigDir "customer-gateway.json")) | Out-Null
    Wait-SampleTcpEndpoint "customer-stream" $CustomerStream
    Wait-SampleTcpEndpoint "customer-mesh" $CustomerMesh

    Start-SampleDotnetAssembly -Name "courier-session" -Project (Join-Path $ScriptDir "Server/CourierSession/DeliveryDispatch.Server.CourierSession.csproj") -LogDirectory $LogDir -Arguments @("--config", (Join-Path $ConfigDir "courier-session.json")) | Out-Null
    Wait-SampleTcpEndpoint "courier-session-stream" $CourierStream
    Wait-SampleTcpEndpoint "courier-session-mesh" $CourierSessionMesh

    Start-SampleDotnetAssembly -Name "courier-actor-node1" -Project (Join-Path $ScriptDir "Server/CourierActorNode/DeliveryDispatch.Server.CourierActorNode.csproj") -LogDirectory $LogDir -Arguments @("--config", (Join-Path $ConfigDir "courier-actor-node1.json")) | Out-Null
    Wait-SampleTcpEndpoint "courier-actor-node1-mesh" $CourierNode1Mesh

    Start-SampleDotnetAssembly -Name "courier-actor-node2" -Project (Join-Path $ScriptDir "Server/CourierActorNode/DeliveryDispatch.Server.CourierActorNode.csproj") -LogDirectory $LogDir -Arguments @("--config", (Join-Path $ConfigDir "courier-actor-node2.json")) | Out-Null
    Wait-SampleTcpEndpoint "courier-actor-node2-mesh" $CourierNode2Mesh

    Start-SampleDotnetAssembly -Name "dispatch" -Project (Join-Path $ScriptDir "Server/Dispatch/DeliveryDispatch.Server.Dispatch.csproj") -LogDirectory $LogDir -Arguments @("--config", (Join-Path $ConfigDir "dispatch.json")) | Out-Null
    Wait-SampleTcpEndpoint "dispatch-mesh" $DispatchMesh
    Wait-SampleHttpHealth "dispatch" $DispatchHttp

    $clientLog = Join-Path $LogDir "client.log"
    Invoke-SampleDotnetRun -Project (Join-Path $ScriptDir "Client/DeliveryDispatch.Client.csproj") -Arguments @("--config", (Join-Path $ConfigDir "client.json")) *> $clientLog
    if (-not (Select-String -Path $clientLog -Pattern "deliverydispatch=completed" -Quiet)) {
        throw "DeliveryDispatch client did not complete."
    }
    if (-not (Select-String -Path $clientLog -Pattern "topology=ready" -Quiet)) {
        throw "DeliveryDispatch client did not print topology marker."
    }
    if (-not (Select-String -Path $clientLog -Pattern "deliverydispatch-reassignment=completed" -Quiet)) {
        throw "DeliveryDispatch client did not verify reassignment."
    }
    Assert-SampleLogContains -LogDirectory $LogDir -Pattern "deliverydispatch tracking: status"
    Assert-SampleLogContains -LogDirectory $LogDir -Pattern "deliverydispatch customer-session: bound customer"
    Assert-SampleLogContains -LogDirectory $LogDir -Pattern "deliverydispatch customer-entry: pushed status"
    Assert-SampleLogContains -LogDirectory $LogDir -Pattern "deliverydispatch courier-session: bound courier=courier-a"
    Assert-SampleLogContains -LogDirectory $LogDir -Pattern "deliverydispatch courier-session: bound courier=courier-b"
    Write-Host "deliverydispatch-runner-evidence=completed"
    $RunSucceeded = $true
}
finally {
    Remove-SampleConfigurationFiles -RunDirectory $RunDir
    Stop-SampleProcesses
    if ($RedisContainer) {
        Remove-SampleRedisContainer $RedisContainer
    }
    if (-not $RunSucceeded -or $DELIVERYDISPATCH_KEEP_RUN_DIR -eq "1") {
        Write-Host "runDir=$RunDir"
    }
    else {
        Remove-Item -Recurse -Force $RunDir -ErrorAction SilentlyContinue
    }
}

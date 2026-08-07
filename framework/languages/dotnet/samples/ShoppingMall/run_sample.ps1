$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptDir "../sample_runner.ps1")

$RunDir = New-SampleRunDirectory "shoppingmall-dotnet"
$RunId = "$PID-$([Guid]::NewGuid().ToString('N'))"
$RedisContainer = $null
$RunSucceeded = $false
$LogDir = Join-Path $RunDir "logs"
$SampleLogDir = Join-Path $RunDir "sample-logs"
New-Item -ItemType Directory -Force -Path $LogDir, $SampleLogDir | Out-Null

try {
    $ports = New-SamplePorts -Count 8 -BasePort 0

    $SHOPPINGMALL_LOG_DIR = $SampleLogDir
    $SHOPPINGMALL_REDIS_KEY_PREFIX = "shoppingmall:dotnet:${RunId}:"
    $SHOPPINGMALL_API_A_HTTP_URL = "http://127.0.0.1:$($ports[0])"
    $SHOPPINGMALL_API_B_HTTP_URL = "http://127.0.0.1:$($ports[1])"
    $SHOPPINGMALL_WORKFLOW_A_HTTP_URL = "http://127.0.0.1:$($ports[2])"
    $SHOPPINGMALL_WORKFLOW_B_HTTP_URL = "http://127.0.0.1:$($ports[3])"
    $SHOPPINGMALL_API_A_MESH_ENDPOINT = "tcp://127.0.0.1:$($ports[4])"
    $SHOPPINGMALL_API_B_MESH_ENDPOINT = "tcp://127.0.0.1:$($ports[5])"
    $SHOPPINGMALL_WORKFLOW_A_MESH_ENDPOINT = "tcp://127.0.0.1:$($ports[6])"
    $SHOPPINGMALL_WORKFLOW_B_MESH_ENDPOINT = "tcp://127.0.0.1:$($ports[7])"

    Invoke-SampleDotnetBuild (Join-Path $ScriptDir "ShoppingMall.csproj")

    $redis = Start-SampleRedisContainer "zlink-shoppingmall-dotnet-redis"
    $RedisContainer = $redis.ContainerId
    $SHOPPINGMALL_REDIS_ENDPOINT = $redis.Endpoint
    Wait-SampleTcpEndpoint "redis" "tcp://$SHOPPINGMALL_REDIS_ENDPOINT"
    $common = @{
        LogDirectory = $SampleLogDir
        RedisEndpoint = $SHOPPINGMALL_REDIS_ENDPOINT
        RedisKeyPrefix = $SHOPPINGMALL_REDIS_KEY_PREFIX
    }
    $configFiles = @{}
    $roles = @{
        "workflow-a" = $common + @{
            InstanceId = "workflow-a"; WorkflowAHttpUrl = $SHOPPINGMALL_WORKFLOW_A_HTTP_URL
            WorkflowAMeshEndpoint = $SHOPPINGMALL_WORKFLOW_A_MESH_ENDPOINT
        }
        "workflow-b" = $common + @{
            InstanceId = "workflow-b"; WorkflowBHttpUrl = $SHOPPINGMALL_WORKFLOW_B_HTTP_URL
            WorkflowBMeshEndpoint = $SHOPPINGMALL_WORKFLOW_B_MESH_ENDPOINT
        }
        "api-a" = $common + @{ InstanceId = "api-a"; ApiAHttpUrl = $SHOPPINGMALL_API_A_HTTP_URL; ApiAMeshEndpoint = $SHOPPINGMALL_API_A_MESH_ENDPOINT }
        "api-b" = $common + @{ InstanceId = "api-b"; ApiBHttpUrl = $SHOPPINGMALL_API_B_HTTP_URL; ApiBMeshEndpoint = $SHOPPINGMALL_API_B_MESH_ENDPOINT }
    }
    foreach ($instance in $roles.Keys) {
        $path = Join-Path $RunDir "appsettings.$instance.json"
        @{ Sample = $roles[$instance] } | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -Path $path
        $configFiles[$instance] = $path
    }
    $clientPath = Join-Path $RunDir "appsettings.client.json"
    @{ Client = @{
        LogDirectory = $SampleLogDir
        ApiAHttpUrl = $SHOPPINGMALL_API_A_HTTP_URL
        ApiBHttpUrl = $SHOPPINGMALL_API_B_HTTP_URL
    } } | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 -Path $clientPath
    $configFiles["client"] = $clientPath

    Start-SampleDotnetAssembly -Name "workflow-a" -Project (Join-Path $ScriptDir "Server/OrderWorkflow/ShoppingMall.OrderWorkflow.csproj") -LogDirectory $LogDir -Arguments @("--config", $configFiles["workflow-a"]) | Out-Null
    Wait-SampleTcpEndpoint "workflow-a-mesh" $SHOPPINGMALL_WORKFLOW_A_MESH_ENDPOINT -Attempts 30
    Wait-SampleHttpHealth "workflow-a" $SHOPPINGMALL_WORKFLOW_A_HTTP_URL -Attempts 30

    Start-SampleDotnetAssembly -Name "workflow-b" -Project (Join-Path $ScriptDir "Server/OrderWorkflow/ShoppingMall.OrderWorkflow.csproj") -LogDirectory $LogDir -Arguments @("--config", $configFiles["workflow-b"]) | Out-Null
    Wait-SampleTcpEndpoint "workflow-b-mesh" $SHOPPINGMALL_WORKFLOW_B_MESH_ENDPOINT -Attempts 30
    Wait-SampleHttpHealth "workflow-b" $SHOPPINGMALL_WORKFLOW_B_HTTP_URL -Attempts 30

    Start-SampleDotnetAssembly -Name "api-a" -Project (Join-Path $ScriptDir "Server/CommerceApi/ShoppingMall.CommerceApi.csproj") -LogDirectory $LogDir -Arguments @("--config", $configFiles["api-a"]) | Out-Null
    Wait-SampleTcpEndpoint "api-a-mesh" $SHOPPINGMALL_API_A_MESH_ENDPOINT -Attempts 30
    Wait-SampleHttpHealth "api-a" $SHOPPINGMALL_API_A_HTTP_URL -Attempts 30

    Start-SampleDotnetAssembly -Name "api-b" -Project (Join-Path $ScriptDir "Server/CommerceApi/ShoppingMall.CommerceApi.csproj") -LogDirectory $LogDir -Arguments @("--config", $configFiles["api-b"]) | Out-Null
    Wait-SampleTcpEndpoint "api-b-mesh" $SHOPPINGMALL_API_B_MESH_ENDPOINT -Attempts 30
    Wait-SampleHttpHealth "api-b" $SHOPPINGMALL_API_B_HTTP_URL -Attempts 30

    # Prepare deterministic recovery fixtures outside the Client process. The
    # Client uses only public order endpoints; these self-check routes are the
    # runner's server observation/setup hook.
    $jsonHeaders = @{ "Content-Type" = "application/json" }
    $pendingBody = @{ idempotencyKey = "order-pending-001"; orderId = "order-pending-0001" } | ConvertTo-Json -Compress
    Invoke-RestMethod -Method Post -Uri "$SHOPPINGMALL_API_A_HTTP_URL/self-check/idempotency/pending" -Headers $jsonHeaders -Body $pendingBody | Out-Null
    $resumeMappingBody = @{ idempotencyKey = "order-resume-001"; orderId = "order-resume-001" } | ConvertTo-Json -Compress
    Invoke-RestMethod -Method Post -Uri "$SHOPPINGMALL_API_A_HTTP_URL/self-check/idempotency/pending" -Headers $jsonHeaders -Body $resumeMappingBody | Out-Null
    $resumeBody = @{
        cartId = "cart-success"; shippingAddressId = "addr-home"; paymentMethodId = "pm-ok"; idempotencyKey = "order-resume-001"
    } | ConvertTo-Json -Compress
    Invoke-RestMethod -Method Post -Uri "$SHOPPINGMALL_API_A_HTTP_URL/self-check/workflow/inventory-reserved" -Headers $jsonHeaders -Body $resumeBody | Out-Null
    $repairMappingBody = @{ idempotencyKey = "order-repair-001"; orderId = "order-repair-001" } | ConvertTo-Json -Compress
    Invoke-RestMethod -Method Post -Uri "$SHOPPINGMALL_API_A_HTTP_URL/self-check/idempotency/pending" -Headers $jsonHeaders -Body $repairMappingBody | Out-Null
    $repairBody = @{
        cartId = "cart-success"; shippingAddressId = "addr-home"; paymentMethodId = "pm-ok"; idempotencyKey = "order-repair-001"
    } | ConvertTo-Json -Compress
    Invoke-RestMethod -Method Post -Uri "$SHOPPINGMALL_API_A_HTTP_URL/self-check/workflow/inventory-reserved" -Headers $jsonHeaders -Body $repairBody | Out-Null
    Invoke-RestMethod -Method Post -Uri "$SHOPPINGMALL_API_A_HTTP_URL/orders/order-repair-001/continue" -Headers $jsonHeaders -Body "{}" | Out-Null
    Invoke-RestMethod -Method Post -Uri "$SHOPPINGMALL_API_A_HTTP_URL/self-check/projection/order-repair-001/delete" -Headers $jsonHeaders -Body "{}" | Out-Null
    $projectionStillExists = $false
    try {
        Invoke-WebRequest -Method Get -Uri "$SHOPPINGMALL_API_A_HTTP_URL/orders/order-repair-001" -UseBasicParsing | Out-Null
        $projectionStillExists = $true
    }
    catch {
    }
    if ($projectionStillExists) {
        throw "Projection deletion fixture was not visible through the public read API."
    }

    Invoke-SampleDotnetRun -Project (Join-Path $ScriptDir "Client/ShoppingMall.Client.csproj") -Arguments @("--config", $configFiles["client"])

    Assert-SampleLogContains -LogDirectory $SampleLogDir -Pattern "shoppingmall=completed"
    $workflowStarted =
        (Select-String -Path (Join-Path $LogDir "workflow-a.out.log") -SimpleMatch "shoppingmall order: started" -Quiet) -or
        (Select-String -Path (Join-Path $LogDir "workflow-b.out.log") -SimpleMatch "shoppingmall order: started" -Quiet)
    if (-not $workflowStarted) {
        throw "No workflow instance recorded a shoppingmall order start."
    }
    $assertionBody = @{
        successfulOrderId = "order-0001"; pendingRecoveredOrderId = "order-pending-0001"; concurrentOrderId = "order-0002"
        resumedOrderId = "order-resume-001"; inventoryFailureOrderId = "order-0003"; paymentFailureOrderId = "order-0004"
        scaleOutOrderId = "order-0005"; repairOrderId = "order-repair-001"
    } | ConvertTo-Json -Compress
    $assertion = Invoke-RestMethod -Method Post -Uri "$SHOPPINGMALL_API_A_HTTP_URL/self-check/assert" -Headers $jsonHeaders -Body $assertionBody
    if (-not $assertion.Passed) {
        throw "ShoppingMall server evidence assertion failed."
    }
    Assert-SampleLogContains -LogDirectory $LogDir -Pattern "shoppingmall evidence:"
    Write-Host "shoppingmall-server-evidence=completed"
    $RunSucceeded = $true
}
finally {
    Remove-SampleConfigurationFiles -RunDirectory $RunDir
    Stop-SampleProcesses
    if ($RedisContainer) {
        Remove-SampleRedisContainer $RedisContainer
    }
    if (-not $RunSucceeded -or $env:SHOPPINGMALL_KEEP_RUN_DIR -eq "1") {
        Write-Host "runDir=$RunDir"
    }
    else {
        Remove-Item -Recurse -Force $RunDir -ErrorAction SilentlyContinue
    }
}

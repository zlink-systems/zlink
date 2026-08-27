$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptDir "../sample_runner.ps1")

$RunDir = New-SampleRunDirectory "shoppingmall-dotnet"
$RunId = "$PID-$([Guid]::NewGuid().ToString('N'))"
$RedisContainer = $null
$RunSucceeded = $false
$WaitAttempts = 300
$LogDir = Join-Path $RunDir "logs"
$SampleLogDir = Join-Path $RunDir "sample-logs"
New-Item -ItemType Directory -Force -Path $LogDir, $SampleLogDir | Out-Null

function Wait-ShoppingMallLogContains {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Pattern
    )

    for ($attempt = 0; $attempt -lt $WaitAttempts; $attempt++) {
        if ((Test-Path $Path) -and (Select-String -Path $Path -SimpleMatch $Pattern -Quiet)) {
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for ${Name}: $Pattern"
}

function Wait-ShoppingMallLogExactCount {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string[]]$Paths,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][int]$Expected
    )

    $actual = 0
    for ($attempt = 0; $attempt -lt $WaitAttempts; $attempt++) {
        $actual = @($Paths | ForEach-Object {
            if (Test-Path $_) {
                Select-String -Path $_ -SimpleMatch $Pattern
            }
        }).Count
        if ($actual -eq $Expected) {
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for ${Name}: expected $Expected lines matching $Pattern, found $actual"
}

function Invoke-ShoppingMallPlannedRelocation {
    param(
        [Parameter(Mandatory = $true)][string]$OrderId,
        [Parameter(Mandatory = $true)][string[]]$WorkflowUrls,
        [Parameter(Mandatory = $true)][hashtable]$Headers
    )

    $lastResult = "no owner observed"
    for ($attempt = 0; $attempt -lt $WaitAttempts; $attempt++) {
        foreach ($url in $WorkflowUrls) {
            try {
                $result = Invoke-RestMethod -Method Post -Uri "$url/self-check/relocate/$OrderId" -Headers $Headers -Body "{}"
            }
            catch {
                continue
            }
            if (-not $result.IsOwner) {
                continue
            }
            if ($result.Outcome -in @("Started", "AlreadyStarted") -and $result.Reason -eq "None") {
                $targetUrl = if ($url -eq $WorkflowUrls[0]) { $WorkflowUrls[1] } else { $WorkflowUrls[0] }
                for ($statusAttempt = 0; $statusAttempt -lt $WaitAttempts; $statusAttempt++) {
                    try {
                        $status = Invoke-RestMethod -Method Get -Uri "$targetUrl/self-check/owner/$OrderId"
                    }
                    catch {
                        Start-Sleep -Milliseconds 100
                        continue
                    }
                    if ($status.IsOwner) {
                        return
                    }
                    Start-Sleep -Milliseconds 100
                }
                throw "Planned relocation did not complete for $OrderId: target did not become owner"
            }
            $lastResult = "owner=true outcome=$($result.Outcome) reason=$($result.Reason)"
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Planned relocation did not complete for $OrderId: $lastResult"
}

function Invoke-ShoppingMallRelocatedOrderContinue {
    param(
        [Parameter(Mandatory = $true)][string]$OrderId,
        [Parameter(Mandatory = $true)][string]$ApiUrl,
        [Parameter(Mandatory = $true)][hashtable]$Headers
    )

    for ($attempt = 0; $attempt -lt $WaitAttempts; $attempt++) {
        try {
            $result = Invoke-RestMethod -Method Post -Uri "$ApiUrl/orders/$OrderId/continue" -Headers $Headers -Body "{}"
            if ($result.state.status -eq "Confirmed") {
                return
            }
        }
        catch { }
        Start-Sleep -Milliseconds 100
    }
    throw "Relocated order did not finish: $OrderId"
}

function Wait-ShoppingMallWorkflowMeshReady {
    param(
        [Parameter(Mandatory = $true)][string[]]$WorkflowUrls
    )

    for ($attempt = 0; $attempt -lt $WaitAttempts; $attempt++) {
        $allReady = $true
        foreach ($url in $WorkflowUrls) {
            try {
                $status = Invoke-RestMethod -Method Get -Uri "$url/self-check/mesh-ready"
                if (-not $status.ready) {
                    $allReady = $false
                }
            }
            catch {
                $allReady = $false
            }
        }
        if ($allReady) {
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for workflow RouteMesh readiness"
}

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
            WorkflowBHttpUrl = $SHOPPINGMALL_WORKFLOW_B_HTTP_URL
            WorkflowBMeshEndpoint = $SHOPPINGMALL_WORKFLOW_B_MESH_ENDPOINT
        }
        "workflow-b" = $common + @{
            InstanceId = "workflow-b"; WorkflowBHttpUrl = $SHOPPINGMALL_WORKFLOW_B_HTTP_URL
            WorkflowBMeshEndpoint = $SHOPPINGMALL_WORKFLOW_B_MESH_ENDPOINT
            WorkflowAHttpUrl = $SHOPPINGMALL_WORKFLOW_A_HTTP_URL
            WorkflowAMeshEndpoint = $SHOPPINGMALL_WORKFLOW_A_MESH_ENDPOINT
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
    Wait-SampleTcpEndpoint "workflow-a-mesh" $SHOPPINGMALL_WORKFLOW_A_MESH_ENDPOINT -Attempts $WaitAttempts
    Wait-SampleHttpHealth "workflow-a" $SHOPPINGMALL_WORKFLOW_A_HTTP_URL -Attempts $WaitAttempts

    Start-SampleDotnetAssembly -Name "workflow-b" -Project (Join-Path $ScriptDir "Server/OrderWorkflow/ShoppingMall.OrderWorkflow.csproj") -LogDirectory $LogDir -Arguments @("--config", $configFiles["workflow-b"]) | Out-Null
    Wait-SampleTcpEndpoint "workflow-b-mesh" $SHOPPINGMALL_WORKFLOW_B_MESH_ENDPOINT -Attempts $WaitAttempts
    Wait-SampleHttpHealth "workflow-b" $SHOPPINGMALL_WORKFLOW_B_HTTP_URL -Attempts $WaitAttempts

    Start-SampleDotnetAssembly -Name "api-a" -Project (Join-Path $ScriptDir "Server/CommerceApi/ShoppingMall.CommerceApi.csproj") -LogDirectory $LogDir -Arguments @("--config", $configFiles["api-a"]) | Out-Null
    Wait-SampleTcpEndpoint "api-a-mesh" $SHOPPINGMALL_API_A_MESH_ENDPOINT -Attempts $WaitAttempts
    Wait-SampleHttpHealth "api-a" $SHOPPINGMALL_API_A_HTTP_URL -Attempts $WaitAttempts

    Start-SampleDotnetAssembly -Name "api-b" -Project (Join-Path $ScriptDir "Server/CommerceApi/ShoppingMall.CommerceApi.csproj") -LogDirectory $LogDir -Arguments @("--config", $configFiles["api-b"]) | Out-Null
    Wait-SampleTcpEndpoint "api-b-mesh" $SHOPPINGMALL_API_B_MESH_ENDPOINT -Attempts $WaitAttempts
    Wait-SampleHttpHealth "api-b" $SHOPPINGMALL_API_B_HTTP_URL -Attempts $WaitAttempts

    # The sample emits this only after its HTTP edge is listening and its RouteMesh
    # has passively observed both workflow peers. The runner sends no readiness request.
    Wait-ShoppingMallLogContains "api-a-http" (Join-Path $LogDir "api-a.out.log") "shoppingmall-ready kind=http node=api-a"
    Wait-ShoppingMallLogContains "api-b-http" (Join-Path $LogDir "api-b.out.log") "shoppingmall-ready kind=http node=api-b"
    Wait-ShoppingMallLogContains "api-a-workflow-a" (Join-Path $LogDir "api-a.out.log") "shoppingmall-ready kind=object-route node=api-a target=workflow-a"
    Wait-ShoppingMallLogContains "api-a-workflow-b" (Join-Path $LogDir "api-a.out.log") "shoppingmall-ready kind=object-route node=api-a target=workflow-b"
    Wait-ShoppingMallLogContains "api-b-workflow-a" (Join-Path $LogDir "api-b.out.log") "shoppingmall-ready kind=object-route node=api-b target=workflow-a"
    Wait-ShoppingMallLogContains "api-b-workflow-b" (Join-Path $LogDir "api-b.out.log") "shoppingmall-ready kind=object-route node=api-b target=workflow-b"

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

    Wait-ShoppingMallLogContains "client-completed" (Join-Path $SampleLogDir "client.log") "shoppingmall=completed"
    Wait-ShoppingMallLogContains "workflow-a-order" (Join-Path $LogDir "workflow-a.out.log") "shoppingmall-order started order="
    Wait-ShoppingMallLogContains "workflow-b-order" (Join-Path $LogDir "workflow-b.out.log") "shoppingmall-order started order="
    $orders = Get-Content -Raw (Join-Path $SampleLogDir "shoppingmall-client-orders.json") | ConvertFrom-Json
    $assertionBody = @{
        successfulOrderId = $orders.SuccessfulOrderId; pendingRecoveredOrderId = $orders.PendingRecoveredOrderId; concurrentOrderId = $orders.ConcurrentOrderId
        resumedOrderId = $orders.ResumedOrderId; inventoryFailureOrderId = $orders.InventoryFailureOrderId; paymentFailureOrderId = $orders.PaymentFailureOrderId
        scaleOutOrderId = $orders.ScaleOutOrderId; repairOrderId = $orders.RepairOrderId
    } | ConvertTo-Json -Compress
    $assertion = Invoke-RestMethod -Method Post -Uri "$SHOPPINGMALL_API_A_HTTP_URL/self-check/assert" -Headers $jsonHeaders -Body $assertionBody
    if (-not $assertion.Passed) {
        throw "ShoppingMall server evidence assertion failed."
    }
    Wait-ShoppingMallLogContains "commerce-evidence" (Join-Path $LogDir "api-a.out.log") "shoppingmall-evidence order="
    Wait-ShoppingMallWorkflowMeshReady -WorkflowUrls @($SHOPPINGMALL_WORKFLOW_A_HTTP_URL, $SHOPPINGMALL_WORKFLOW_B_HTTP_URL)
    # The runner creates and checkpoints this order, relocates its actual owner,
    # then lets the public continue endpoint drive the target's next step.
    $relocationBody = @{
        cartId = "cart-success"; shippingAddressId = "addr-home"; paymentMethodId = "pm-ok"
        idempotencyKey = "order-relocation-$RunId"
    } | ConvertTo-Json -Compress
    $relocationCheckpoint = Invoke-RestMethod -Method Post -Uri "$SHOPPINGMALL_API_A_HTTP_URL/self-check/workflow/inventory-reserved" -Headers $jsonHeaders -Body $relocationBody
    $relocationOrderId = $relocationCheckpoint.orderId
    Invoke-RestMethod -Method Post -Uri "$SHOPPINGMALL_API_A_HTTP_URL/self-check/relocation/$relocationOrderId/arm" -Headers $jsonHeaders -Body "{}" | Out-Null
    Invoke-ShoppingMallPlannedRelocation -OrderId $relocationOrderId -WorkflowUrls @($SHOPPINGMALL_WORKFLOW_A_HTTP_URL, $SHOPPINGMALL_WORKFLOW_B_HTTP_URL) -Headers $jsonHeaders
    Invoke-ShoppingMallRelocatedOrderContinue -OrderId $relocationOrderId -ApiUrl $SHOPPINGMALL_API_A_HTTP_URL -Headers $jsonHeaders
    # Planned relocation is intentionally required. These can pass only when the
    # sample actually drives relocation; store wiring does not emit either line.
    Wait-ShoppingMallLogExactCount "replayed" @((Join-Path $LogDir "workflow-a.out.log"), (Join-Path $LogDir "workflow-b.out.log")) "shoppingmall-order replayed order=" 1
    Wait-ShoppingMallLogExactCount "no-external-effect-repeat" @((Join-Path $LogDir "workflow-a.out.log"), (Join-Path $LogDir "workflow-b.out.log")) "shoppingmall-order external-effect-repeated order=" 0
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
    if ($RunSucceeded) {
        Write-Host "shoppingmall-placement=completed"
    }
}

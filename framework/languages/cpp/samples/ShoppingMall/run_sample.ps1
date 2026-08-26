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
$RunDir = Join-Path ([System.IO.Path]::GetTempPath()) "shoppingmall-cpp-$PID-$([Guid]::NewGuid().ToString('N'))"
$LogDir = Join-Path $RunDir "logs"
$FlowLogDir = Join-Path $RunDir "flow-logs"
$ConfigDir = Join-Path $RunDir "config"
New-Item -ItemType Directory -Force -Path $LogDir, $FlowLogDir, $ConfigDir | Out-Null

function Find-Binary([string]$Name) {
    foreach ($candidate in @(
        (Join-Path $BuildDir $Name), (Join-Path $BuildDir "$Name.exe"),
        (Join-Path $BuildDir "linux-ninja-debug/$Name"),
        (Join-Path $BuildDir "linux-ninja-debug/$Name.exe")
    )) {
        if (Test-Path $candidate) { return $candidate }
    }
    throw "Missing executable: $Name"
}

function Role-Logs([string]$Name) {
    return @((Join-Path $LogDir "$Name.stdout.log"), (Join-Path $LogDir "$Name.stderr.log"))
}

function Get-ExactLineCount([string[]]$Paths, [string]$ExpectedLine) {
    $count = 0
    foreach ($path in $Paths) {
        if (Test-Path $path) {
            $count += @(Get-Content -Path $path | Where-Object { $_ -ceq $ExpectedLine }).Count
        }
    }
    return $count
}

function Get-PrefixCount([string[]]$Paths, [string]$Prefix) {
    $count = 0
    foreach ($path in $Paths) {
        if (Test-Path $path) {
            $count += @(Get-Content -Path $path | Where-Object {
                $_.StartsWith($Prefix, [System.StringComparison]::Ordinal)
            }).Count
        }
    }
    return $count
}

function Get-PatternCount([string[]]$Paths, [string]$Pattern) {
    $count = 0
    foreach ($path in $Paths) {
        if (Test-Path $path) { $count += @(Select-String -Path $path -Pattern $Pattern).Count }
    }
    return $count
}

function Wait-ExactLine([string]$Name, [string[]]$Paths, [string]$Line, [int]$Expected) {
    $actual = 0
    for ($attempt = 0; $attempt -lt $WaitAttempts; $attempt++) {
        $actual = Get-ExactLineCount $Paths $Line
        if ($actual -eq $Expected) { return }
        if ($actual -gt $Expected) { break }
        Start-Sleep -Milliseconds $WaitMilliseconds
    }
    throw "Expected $Name exactly $Expected time(s), found $actual."
}

function Wait-PrefixMinimum([string]$Name, [string[]]$Paths, [string]$Prefix, [int]$Minimum) {
    $actual = 0
    for ($attempt = 0; $attempt -lt $WaitAttempts; $attempt++) {
        $actual = Get-PrefixCount $Paths $Prefix
        if ($actual -ge $Minimum) { return }
        Start-Sleep -Milliseconds $WaitMilliseconds
    }
    throw "Expected $Name at least $Minimum time(s), found $actual."
}

function Wait-PrefixExact([string]$Name, [string[]]$Paths, [string]$Prefix, [int]$Expected) {
    $actual = 0
    for ($attempt = 0; $attempt -lt $WaitAttempts; $attempt++) {
        $actual = Get-PrefixCount $Paths $Prefix
        if ($actual -eq $Expected) { return }
        if ($actual -gt $Expected) { break }
        Start-Sleep -Milliseconds $WaitMilliseconds
    }
    throw "Expected $Name exactly $Expected time(s), found $actual."
}

function Wait-PatternMinimum([string]$Name, [string[]]$Paths, [string]$Pattern, [int]$Minimum) {
    $actual = 0
    for ($attempt = 0; $attempt -lt $WaitAttempts; $attempt++) {
        $actual = Get-PatternCount $Paths $Pattern
        if ($actual -ge $Minimum) { return }
        Start-Sleep -Milliseconds $WaitMilliseconds
    }
    throw "Expected $Name at least $Minimum time(s), found $actual."
}

function Start-Role([string]$Name, [string]$Binary, [string[]]$Arguments) {
    $process = Start-Process -FilePath $Binary -ArgumentList $Arguments -PassThru `
        -RedirectStandardOutput (Join-Path $LogDir "$Name.stdout.log") `
        -RedirectStandardError (Join-Path $LogDir "$Name.stderr.log")
    $Processes.Add($process)
}

function Write-RoleConfig([string]$RoleName) {
    $configuration = @{ sample = @{ role = @{ name = $RoleName; logDir = $FlowLogDir }; topology = @{
        redisEndpoint = $RedisEndpoint; redisKeyPrefix = $RedisKeyPrefix
        apiAHttpUrl = $ApiAHttpUrl; apiBHttpUrl = $ApiBHttpUrl
        apiARouteEndpoint = $ApiARoute; apiBRouteEndpoint = $ApiBRoute
        apiASpotRouterEndpoint = $ApiASpotRouter; apiBSpotRouterEndpoint = $ApiBSpotRouter
        workflowAHttpUrl = $WorkflowAHttpUrl; workflowBHttpUrl = $WorkflowBHttpUrl
        workflowARouteEndpoint = $WorkflowARoute; workflowBRouteEndpoint = $WorkflowBRoute
        workflowASpotRouteEndpoint = $WorkflowASpotRoute; workflowBSpotRouteEndpoint = $WorkflowBSpotRoute
        workflowASpotEndpoint = $WorkflowASpot; workflowASpotRouterEndpoint = $WorkflowASpotRouter
        workflowBSpotEndpoint = $WorkflowBSpot; workflowBSpotRouterEndpoint = $WorkflowBSpotRouter
    } } }
    $configuration | ConvertTo-Json -Depth 5 | Set-Content -Path (Join-Path $ConfigDir "$RoleName.json") -Encoding utf8
}

function Invoke-Json([string]$BaseUrl, [string]$Path, $Body) {
    return Invoke-RestMethod -Method Post -Uri "$BaseUrl$Path" -ContentType "application/json" `
        -Body ($Body | ConvertTo-Json -Compress) -TimeoutSec 10
}

function Wait-OrderStatus([string]$BaseUrl, [string]$OrderId, [string]$Expected) {
    $status = ""
    for ($attempt = 0; $attempt -lt $WaitAttempts; $attempt++) {
        try {
            $status = (Invoke-Json $BaseUrl "/orders/get" @{ orderId = $OrderId }).state.status
            if ($status -ceq $Expected) { return }
        } catch {
        }
        Start-Sleep -Milliseconds $WaitMilliseconds
    }
    throw "Timed out waiting for order $OrderId to reach $Expected (last status=$status)."
}

function Get-ProducedOrder([string]$Name) {
    $lines = @(Get-Content -Path (Join-Path $LogDir "client.log") | Where-Object {
        $_ -cmatch "^shoppingmall-produced name=$([regex]::Escape($Name)) order=(.+)$"
    })
    if ($lines.Count -ne 1) { throw "Expected one Client-produced order for $Name, found $($lines.Count)." }
    return ($lines[0] -replace '^.* order=', '')
}

function Cleanup {
    foreach ($process in @($Processes)) {
        try {
            if (-not $process.HasExited) { Stop-Process -Id $process.Id -ErrorAction SilentlyContinue }
            [void]$process.WaitForExit(1000)
        } catch {
        }
    }
    if ($RedisContainer) { Remove-ZlinkSampleRedis $RedisContainer }
    if (Test-Path $RunDir) { Remove-Item -Recurse -Force $RunDir }
}

$Succeeded = $false
try {
    & cmake --build $BuildDir --parallel 2 --target `
        sample_cpp_framework_shoppingmall_commerce_api `
        sample_cpp_framework_shoppingmall_order_workflow `
        sample_cpp_framework_shoppingmall_client
    if ($LASTEXITCODE -ne 0) { throw "ShoppingMall sample build failed." }

    $ports = @(Get-ZlinkSamplePorts -Count 17)
    $ApiAHttpUrl = "http://127.0.0.1:$($ports[1])"; $ApiBHttpUrl = "http://127.0.0.1:$($ports[2])"
    $ApiARoute = "tcp://127.0.0.1:$($ports[3])"; $ApiBRoute = "tcp://127.0.0.1:$($ports[4])"
    $WorkflowAHttpUrl = "http://127.0.0.1:$($ports[5])"; $WorkflowBHttpUrl = "http://127.0.0.1:$($ports[6])"
    $WorkflowARoute = "tcp://127.0.0.1:$($ports[7])"; $WorkflowBRoute = "tcp://127.0.0.1:$($ports[8])"
    $WorkflowASpotRoute = "tcp://127.0.0.1:$($ports[9])"; $WorkflowBSpotRoute = "tcp://127.0.0.1:$($ports[10])"
    $WorkflowASpot = "tcp://127.0.0.1:$($ports[11])"; $WorkflowASpotRouter = "tcp://127.0.0.1:$($ports[12])"
    $WorkflowBSpot = "tcp://127.0.0.1:$($ports[13])"; $WorkflowBSpotRouter = "tcp://127.0.0.1:$($ports[14])"
    $ApiASpotRouter = "tcp://127.0.0.1:$($ports[15])"; $ApiBSpotRouter = "tcp://127.0.0.1:$($ports[16])"

    $redis = Start-ZlinkSampleRedis "zlink-redis-cpp-sample-shoppingmall" "redis:7-alpine"
    $RedisContainer = $redis.ContainerId
    $RedisEndpoint = "tcp://$($redis.Endpoint)"
    $RedisKeyPrefix = "shoppingmall:cpp:$PID:$([Guid]::NewGuid().ToString('N')):"
    Write-RoleConfig "workflow-a"; Write-RoleConfig "workflow-b"; Write-RoleConfig "api-a"; Write-RoleConfig "api-b"

    $WorkflowBin = Find-Binary "sample_cpp_framework_shoppingmall_order_workflow"
    $ApiBin = Find-Binary "sample_cpp_framework_shoppingmall_commerce_api"
    $ClientBin = Find-Binary "sample_cpp_framework_shoppingmall_client"
    Start-Role "workflow-a" $WorkflowBin @("--config=$(Join-Path $ConfigDir 'workflow-a.json')")
    Start-Role "workflow-b" $WorkflowBin @("--config=$(Join-Path $ConfigDir 'workflow-b.json')")
    Start-Role "api-a" $ApiBin @("--config=$(Join-Path $ConfigDir 'api-a.json')")
    Start-Role "api-b" $ApiBin @("--config=$(Join-Path $ConfigDir 'api-b.json')")

    Wait-ExactLine "api-a HTTP readiness" (Role-Logs "api-a") "shoppingmall-ready kind=http node=api-a" 1
    Wait-ExactLine "api-b HTTP readiness" (Role-Logs "api-b") "shoppingmall-ready kind=http node=api-b" 1
    Wait-ExactLine "api-a workflow-a object route" (Role-Logs "api-a") "shoppingmall-ready kind=object-route node=api-a target=workflow-a" 1
    Wait-ExactLine "api-a workflow-b object route" (Role-Logs "api-a") "shoppingmall-ready kind=object-route node=api-a target=workflow-b" 1
    Wait-ExactLine "api-b workflow-a object route" (Role-Logs "api-b") "shoppingmall-ready kind=object-route node=api-b target=workflow-a" 1
    Wait-ExactLine "api-b workflow-b object route" (Role-Logs "api-b") "shoppingmall-ready kind=object-route node=api-b target=workflow-b" 1

    $pendingOrderId = (Invoke-Json $ApiAHttpUrl "/self-check/idempotency/pending" @{ idempotencyKey = "order-pending-001"; orderId = "" }).orderId
    $resumeOrderId = (Invoke-Json $ApiAHttpUrl "/self-check/workflow/inventory-reserved" @{ cartId = "cart-success"; shippingAddressId = "addr-home"; paymentMethodId = "pm-ok"; idempotencyKey = "order-resume-001" }).orderId
    $projectionContinueOrderId = (Invoke-Json $ApiAHttpUrl "/orders/start" @{ cartId = "cart-success"; shippingAddressId = "addr-home"; paymentMethodId = "pm-ok"; idempotencyKey = "order-projection-continue-001" }).orderId
    Wait-OrderStatus $ApiAHttpUrl $projectionContinueOrderId "Confirmed"
    [void](Invoke-Json $ApiAHttpUrl "/self-check/projection/delete" @{ orderId = $projectionContinueOrderId })
    $projectionRebuildOrderId = (Invoke-Json $ApiBHttpUrl "/orders/start" @{ cartId = "cart-success"; shippingAddressId = "addr-home"; paymentMethodId = "pm-ok"; idempotencyKey = "order-projection-rebuild-001" }).orderId
    Wait-OrderStatus $ApiBHttpUrl $projectionRebuildOrderId "Confirmed"
    [void](Invoke-Json $ApiBHttpUrl "/self-check/projection/delete" @{ orderId = $projectionRebuildOrderId })

    $clientLog = Join-Path $LogDir "client.log"
    & $ClientBin --api-a-http-url $ApiAHttpUrl --api-b-http-url $ApiBHttpUrl `
        --resume-order-id $resumeOrderId --projection-continue-order-id $projectionContinueOrderId `
        --projection-rebuild-order-id $projectionRebuildOrderId *> $clientLog
    if ($LASTEXITCODE -ne 0) { throw "ShoppingMall client failed." }
    Wait-ExactLine "Client completion marker" @($clientLog) "shoppingmall=completed" 1

    $successOrderId = Get-ProducedOrder "success"; $concurrentOrderId = Get-ProducedOrder "concurrent"
    $clientPendingOrderId = Get-ProducedOrder "pending"; $inventoryFailureOrderId = Get-ProducedOrder "inventory-failure"
    $paymentFailureOrderId = Get-ProducedOrder "payment-failure"; $scaleOutOrderId = Get-ProducedOrder "scale-out"
    if ($clientPendingOrderId -cne $pendingOrderId) { throw "Client pending order did not use this run's prepared identity." }

    # The Client uses only public order endpoints.  The runner creates the
    # checkpoint fixture, asks its owning workflow host to relocate, then waits
    # for the target lifecycle to resume it.
    $plannedRelocationOrderId = (Invoke-Json $ApiAHttpUrl "/self-check/workflow/inventory-reserved" @{
        cartId = "cart-success"; shippingAddressId = "addr-home"; paymentMethodId = "pm-ok"
        idempotencyKey = "order-planned-relocation-001"
    }).orderId
    $relocationTrigger = Invoke-Json $WorkflowAHttpUrl "/self-check/relocation" @{ orderId = $plannedRelocationOrderId }
    if (-not $relocationTrigger.accepted) {
        $relocationTrigger = Invoke-Json $WorkflowBHttpUrl "/self-check/relocation" @{ orderId = $plannedRelocationOrderId }
    }
    if (-not $relocationTrigger.accepted) { throw "Planned relocation trigger did not find the active workflow spot." }
    # The relocated workflow fixture resumes from its target lifecycle and
    # finishes the order itself; the runner does not issue a continuation.
    Wait-OrderStatus $ApiAHttpUrl $plannedRelocationOrderId "Confirmed"

    $assertion = Invoke-Json $ApiAHttpUrl "/self-check/assert" @{
        successfulOrderId = $successOrderId; pendingRecoveredOrderId = $clientPendingOrderId
        concurrentOrderId = $concurrentOrderId; resumedOrderId = $resumeOrderId
        inventoryFailureOrderId = $inventoryFailureOrderId; paymentFailureOrderId = $paymentFailureOrderId
        scaleOutOrderId = $scaleOutOrderId
    }
    if (-not $assertion.passed) { throw "ShoppingMall server assertion failed." }

    Wait-PatternMinimum "workflow-a order start" (Role-Logs "workflow-a") '^shoppingmall-order started order=.+ spot=.+$' 1
    Wait-PatternMinimum "workflow-b order start" (Role-Logs "workflow-b") '^shoppingmall-order started order=.+ spot=.+$' 1
    Wait-PrefixMinimum "CommerceApi evidence" (Role-Logs "api-a") "shoppingmall-evidence order=$successOrderId events=" 1
    Wait-PrefixExact "planned-relocation replay" @((Role-Logs "workflow-a") + (Role-Logs "workflow-b")) "shoppingmall-order replayed order=" 1
    Wait-PrefixExact "repeated external effect" @((Role-Logs "workflow-a") + (Role-Logs "workflow-b")) "shoppingmall-order external-effect-repeated order=" 0
    $Succeeded = $true
} finally {
    Cleanup
}

if (-not $Succeeded) { exit 1 }
Write-Host "shoppingmall-placement=completed"

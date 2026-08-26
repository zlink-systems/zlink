Set-StrictMode -Version Latest
. "$PSScriptRoot/../../redis-common.ps1"
$ErrorActionPreference = "Stop"
$SampleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $SampleDir
$LogDir = Join-Path $SampleDir "build/sample-logs"
$StoreDir = Join-Path $SampleDir "build/sample-store"
$ConfigDir = Join-Path $SampleDir "build/sample-config"
$WaitAttempts = 300
$WaitIntervalMilliseconds = 100
$Processes = [System.Collections.Generic.List[System.Diagnostics.Process]]::new()
$RedisContainerId = $null
$Gradle = if ($IsWindows) { Join-Path $SampleDir "../../gradlew.bat" } else { Join-Path $SampleDir "../../gradlew" }
New-Item -ItemType Directory -Force -Path $LogDir, $StoreDir, $ConfigDir | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $LogDir "*.log"), (Join-Path $StoreDir "*"), (Join-Path $ConfigDir "*.properties")

function Cleanup { param([int]$Status)
    if ($Status -ne 0) { Get-ChildItem $LogDir -Filter "*.log" -ErrorAction SilentlyContinue | ForEach-Object { Get-Content $_.FullName -Tail 200 -ErrorAction SilentlyContinue } }
    for ($i = $Processes.Count - 1; $i -ge 0; $i--) { if (-not $Processes[$i].HasExited) { Stop-Process -Id $Processes[$i].Id -Force -ErrorAction SilentlyContinue } }
    if ($RedisContainerId) { Remove-ZlinkSampleRedis $RedisContainerId }
}
function Split-Endpoint([string]$Endpoint) { $parts = $Endpoint.Split(":"); @{ Host = $parts[0]; Port = [int]$parts[1] } }
function Wait-Port([hashtable]$Endpoint) {
    for ($attempt = 1; $attempt -le $WaitAttempts; $attempt++) {
        $client = [System.Net.Sockets.TcpClient]::new(); try { $task = $client.ConnectAsync($Endpoint.Host, $Endpoint.Port); if ($task.Wait(100)) { return } } catch {} finally { $client.Dispose() }
        Start-Sleep -Milliseconds $WaitIntervalMilliseconds
    }; throw "Timed out waiting for $($Endpoint.Host):$($Endpoint.Port)"
}
function Wait-Http([string]$Base) { for ($attempt = 1; $attempt -le $WaitAttempts; $attempt++) { try { Invoke-WebRequest -UseBasicParsing "$Base/health" | Out-Null; return } catch {}; Start-Sleep -Milliseconds $WaitIntervalMilliseconds }; throw "Timed out waiting for $Base/health" }
function Get-LogCount([string]$Path, [string]$Line) { if (-not (Test-Path $Path)) { return 0 }; @((Select-String -Path $Path -Pattern $Line -SimpleMatch -ErrorAction SilentlyContinue)).Count }
function Wait-LogCount([string]$Path, [string]$Line, [int]$Expected) { for ($attempt = 1; $attempt -le $WaitAttempts; $attempt++) { if ((Get-LogCount $Path $Line) -eq $Expected) { return }; Start-Sleep -Milliseconds $WaitIntervalMilliseconds }; throw "Timed out waiting for $Expected '$Line' in $Path" }
function Wait-LogAtLeast([string]$Path, [string]$Line, [int]$Expected) { for ($attempt = 1; $attempt -le $WaitAttempts; $attempt++) { if ((Get-LogCount $Path $Line) -ge $Expected) { return }; Start-Sleep -Milliseconds $WaitIntervalMilliseconds }; throw "Timed out waiting for $Expected+ '$Line' in $Path" }
function Wait-ReplayOnce([string]$Line, [string]$A, [string]$B) { for ($attempt = 1; $attempt -le $WaitAttempts; $attempt++) { $count = (Get-LogCount $A $Line) + (Get-LogCount $B $Line); if ($count -eq 1) { return }; if ($count -gt 1) { throw "Found $count '$Line'" }; Start-Sleep -Milliseconds $WaitIntervalMilliseconds }; throw "Timed out waiting for one '$Line'" }
function Wait-LogTotalAtLeast([string]$Line, [int]$Expected, [string[]]$Paths) { for ($attempt = 1; $attempt -le $WaitAttempts; $attempt++) { $count = 0; foreach ($path in $Paths) { $count += Get-LogCount $path $Line }; if ($count -ge $Expected) { return }; Start-Sleep -Milliseconds $WaitIntervalMilliseconds }; throw "Timed out waiting for $Expected+ '$Line' across logs" }
function Start-Role([string]$Project, [string]$Name, [string]$Config, [string]$LogName) { $script = if ($IsWindows) { "$Name.bat" } else { $Name }; $bin = Join-Path $SampleDir "$Project/build/install/$Name/bin/$script"; $process = Start-Process -FilePath $bin -ArgumentList @("--config", $Config) -WorkingDirectory $SampleDir -NoNewWindow -RedirectStandardOutput (Join-Path $LogDir $LogName) -RedirectStandardError (Join-Path $LogDir "$LogName.err.log") -PassThru; $Processes.Add($process); return $process }
function Invoke-JsonPost([string]$Url, [object]$Body) { Invoke-RestMethod -Method Post -Uri $Url -ContentType "application/json" -Body ($Body | ConvertTo-Json -Compress) }
function Get-ClientOrder([string]$Path, [string]$Name) { $match = Select-String -Path $Path -Pattern ("^shoppingmall-client-order name=" + $Name + " order=([^\s]+)$"); if ($null -eq $match) { return $null }; $match.Matches[0].Groups[1].Value }
function Wait-OrderStatus([string]$Base, [string]$OrderId, [string]$Status) { for ($attempt = 1; $attempt -le $WaitAttempts; $attempt++) { try { if ((Invoke-RestMethod -Method Get -Uri "$Base/orders/$OrderId").state.status -eq $Status) { return } } catch {}; Start-Sleep -Milliseconds $WaitIntervalMilliseconds }; throw "Timed out waiting for $OrderId to reach $Status" }

$Status = 1
try {
    $endpoints = @(Get-ZlinkSampleApplicationEndpoints -Language Kotlin -Count 8)
    $apiAChannel = Split-Endpoint $endpoints[0]; $apiBChannel = Split-Endpoint $endpoints[1]; $workflowAChannel = Split-Endpoint $endpoints[2]; $workflowBChannel = Split-Endpoint $endpoints[3]
    $apiAHttp = Split-Endpoint $endpoints[4]; $apiBHttp = Split-Endpoint $endpoints[5]; $workflowAHttp = Split-Endpoint $endpoints[6]; $workflowBHttp = Split-Endpoint $endpoints[7]
    $redis = Start-ZlinkSampleRedis "zlink-redis-kotlin-sample-shoppingmall" -Language Kotlin; $RedisContainerId = $redis.ContainerId
    $redisEndpoint = $redis.Endpoint; Wait-Port (Split-Endpoint $redisEndpoint)
    $prefix = "shoppingmall:kotlin:${PID}:$([Guid]::NewGuid().ToString('N')):"
    $apiAHttpUrl = "http://$($apiAHttp.Host):$($apiAHttp.Port)"; $apiBHttpUrl = "http://$($apiBHttp.Host):$($apiBHttp.Port)"; $workflowAHttpUrl = "http://$($workflowAHttp.Host):$($workflowAHttp.Port)"; $workflowBHttpUrl = "http://$($workflowBHttp.Host):$($workflowBHttp.Port)"
    $writeRole = { param($path, $id, $channel, $http) @("sample.instanceId=$id", "sample.httpEndpoint=$http", "sample.logDirectory=$LogDir", "sample.channelEndpoint=$channel", "sample.redisEndpoint=$redisEndpoint", "sample.redisKeyPrefix=$prefix", "sample.storeDirectory=$StoreDir") | Set-Content $path -Encoding UTF8 }
    $workflowAConfig = Join-Path $ConfigDir "workflow-a.properties"; $workflowBConfig = Join-Path $ConfigDir "workflow-b.properties"; $apiAConfig = Join-Path $ConfigDir "api-a.properties"; $apiBConfig = Join-Path $ConfigDir "api-b.properties"; $clientConfig = Join-Path $ConfigDir "client.properties"
    & $writeRole $workflowAConfig "workflow-a" "tcp://$($workflowAChannel.Host):$($workflowAChannel.Port)" $workflowAHttpUrl; & $writeRole $workflowBConfig "workflow-b" "tcp://$($workflowBChannel.Host):$($workflowBChannel.Port)" $workflowBHttpUrl; & $writeRole $apiAConfig "api-a" "tcp://$($apiAChannel.Host):$($apiAChannel.Port)" $apiAHttpUrl; & $writeRole $apiBConfig "api-b" "tcp://$($apiBChannel.Host):$($apiBChannel.Port)" $apiBHttpUrl
    @("sample.apiAHttpUrl=$apiAHttpUrl", "sample.apiBHttpUrl=$apiBHttpUrl") | Set-Content $clientConfig -Encoding UTF8
    Push-Location "../../.."; try { Invoke-ZlinkSampleGradleBuild -GradleExecutable $Gradle -Arguments @("--no-daemon", "--no-parallel", "--max-workers=1", ":zlink-framework-core:jar", ":zlink-framework-spring-boot-starter:jar", ":zlink-framework-kotlin:jar", ":zlink-framework-locations-redis:jar", "--quiet") } finally { Pop-Location }
    Invoke-ZlinkSampleGradleBuild -GradleExecutable $Gradle -Arguments @("-c", "standalone.settings.gradle.kts", "--no-daemon", "--no-parallel", "--max-workers=1", ":Server:OrderWorkflow:installDist", ":Server:CommerceApi:installDist", ":Client:installDist", "--quiet")
    Start-Role "Server/OrderWorkflow" "OrderWorkflow" $workflowAConfig "workflow-a.log" | Out-Null; Start-Role "Server/OrderWorkflow" "OrderWorkflow" $workflowBConfig "workflow-b.log" | Out-Null; Wait-Port $workflowAChannel; Wait-Port $workflowBChannel; Wait-Http $workflowAHttpUrl; Wait-Http $workflowBHttpUrl
    Start-Role "Server/CommerceApi" "CommerceApi" $apiAConfig "api-a.log" | Out-Null; Start-Role "Server/CommerceApi" "CommerceApi" $apiBConfig "api-b.log" | Out-Null; Wait-Port $apiAChannel; Wait-Port $apiBChannel; Wait-Http $apiAHttpUrl; Wait-Http $apiBHttpUrl
    $workflowALog = Join-Path $LogDir "workflow-a.log"; $workflowBLog = Join-Path $LogDir "workflow-b.log"; $apiALog = Join-Path $LogDir "api-a.log"; $apiBLog = Join-Path $LogDir "api-b.log"
    Wait-LogCount $apiALog "shoppingmall-ready kind=http node=api-a" 1; Wait-LogCount $apiBLog "shoppingmall-ready kind=http node=api-b" 1
    foreach ($api in @("a", "b")) { foreach ($workflow in @("a", "b")) { Wait-LogCount (Join-Path $LogDir "api-$api.log") "shoppingmall-ready kind=object-route node=api-$api target=workflow-$workflow" 1 } }
    $pendingKey = "$prefix`runner-pending"; $pending = Invoke-JsonPost "$apiAHttpUrl/self-check/idempotency/pending" @{ cartId="cart-success"; shippingAddressId="addr-home"; paymentMethodId="pm-ok"; idempotencyKey=$pendingKey }
    for ($attempt = 1; $attempt -le 20 -and ((Get-LogCount $workflowALog "shoppingmall-order started order=") -lt 1 -or (Get-LogCount $workflowBLog "shoppingmall-order started order=") -lt 1); $attempt++) { Invoke-JsonPost "$apiAHttpUrl/orders/start" @{ cartId="cart-inventory-fail"; shippingAddressId="addr-home"; paymentMethodId="pm-ok"; idempotencyKey="$prefix`runner-witness-$attempt" } | Out-Null; Start-Sleep -Milliseconds $WaitIntervalMilliseconds }
    Wait-LogAtLeast $workflowALog "shoppingmall-order started order=" 1; Wait-LogAtLeast $workflowBLog "shoppingmall-order started order=" 1
    $checkpoint = Invoke-JsonPost "$apiAHttpUrl/self-check/workflow/inventory-reserved" @{ cartId="cart-success"; shippingAddressId="addr-office"; paymentMethodId="pm-ok"; idempotencyKey="$prefix`runner-relocation" }; if ([string]::IsNullOrWhiteSpace($checkpoint.orderId) -or $null -eq $checkpoint.objectGeneration) { throw "Runner relocation fixture did not report its order and generation." }
    $rebuild = Invoke-JsonPost "$apiAHttpUrl/orders/start" @{ cartId="cart-success"; shippingAddressId="addr-home"; paymentMethodId="pm-ok"; idempotencyKey="$prefix`runner-rebuild" }; Wait-OrderStatus $apiAHttpUrl $rebuild.orderId "Confirmed"; Invoke-JsonPost "$apiAHttpUrl/self-check/projection/$($rebuild.orderId)/delete" @{} | Out-Null
    $source = $null; for ($attempt = 1; $attempt -le $WaitAttempts; $attempt++) { if ((Get-LogCount $workflowALog "shoppingmall-order started order=$($checkpoint.orderId) ") -ge 1) { $source = $workflowAHttpUrl; break }; if ((Get-LogCount $workflowBLog "shoppingmall-order started order=$($checkpoint.orderId) ") -ge 1) { $source = $workflowBHttpUrl; break }; Start-Sleep -Milliseconds $WaitIntervalMilliseconds }; if ($null -eq $source) { throw "Runner could not locate the relocation source workflow." }; if ((Invoke-JsonPost "$source/self-check/relocate" @{}).outcome -ne "RELOCATED") { throw "Planned relocation did not complete." }
    @("sample.apiAHttpUrl=$apiAHttpUrl", "sample.apiBHttpUrl=$apiBHttpUrl", "sample.pendingIdempotencyKey=$pendingKey", "sample.pendingOrderId=$($pending.orderId)", "sample.resumeOrderId=$($checkpoint.orderId)", "sample.rebuildOrderId=$($rebuild.orderId)") | Set-Content $clientConfig -Encoding UTF8
    $client = Start-Role "Client" "Client" $clientConfig "client.log"; $client.WaitForExit(); if ($client.ExitCode -ne 0) { throw "Client failed" }; $clientLog = Join-Path $LogDir "client.log"; Wait-LogCount $clientLog "shoppingmall=completed" 1
    $success = Get-ClientOrder $clientLog "success"; $concurrent = Get-ClientOrder $clientLog "concurrent"; $inventory = Get-ClientOrder $clientLog "inventory-failure"; $payment = Get-ClientOrder $clientLog "payment-failure"; $scale = Get-ClientOrder $clientLog "scale-out"; if (@($success, $concurrent, $inventory, $payment, $scale).Where({ [string]::IsNullOrWhiteSpace($_) }).Count -ne 0) { throw "Client did not report its produced order ID." }
    $assertion = Invoke-JsonPost "$apiAHttpUrl/self-check/assert" @{ successfulOrderId=$success; pendingRecoveredOrderId=$pending.orderId; concurrentOrderId=$concurrent; resumedOrderId=$checkpoint.orderId; inventoryFailureOrderId=$inventory; paymentFailureOrderId=$payment; scaleOutOrderId=$scale }; if (-not $assertion.passed) { throw "Server assertion failed." }
    Wait-LogTotalAtLeast "shoppingmall-evidence order=$($checkpoint.orderId) events=" 1 @($apiALog, $apiBLog); Wait-ReplayOnce "shoppingmall-order replayed order=$($checkpoint.orderId) generation=$($checkpoint.objectGeneration)" $workflowALog $workflowBLog; Wait-LogCount $workflowALog "shoppingmall-order external-effect-repeated order=$($checkpoint.orderId)" 0; Wait-LogCount $workflowBLog "shoppingmall-order external-effect-repeated order=$($checkpoint.orderId)" 0
    $Status = 0
} finally { Cleanup $Status }
if ($Status -eq 0) { Write-Output "shoppingmall-placement=completed" }

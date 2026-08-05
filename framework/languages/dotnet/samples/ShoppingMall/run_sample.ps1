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

    Invoke-SampleDotnetRun -Project (Join-Path $ScriptDir "Client/ShoppingMall.Client.csproj") -Arguments @("--config", $configFiles["client"])

    Assert-SampleLogContains -LogDirectory $SampleLogDir -Pattern "shoppingmall=completed"
    $workflowStarted =
        (Select-String -Path (Join-Path $LogDir "workflow-a.out.log") -SimpleMatch "shoppingmall order: started" -Quiet) -or
        (Select-String -Path (Join-Path $LogDir "workflow-b.out.log") -SimpleMatch "shoppingmall order: started" -Quiet)
    if (-not $workflowStarted) {
        throw "No workflow instance recorded a shoppingmall order start."
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
    if (-not $RunSucceeded -or $SHOPPINGMALL_KEEP_RUN_DIR -eq "1") {
        Write-Host "runDir=$RunDir"
    }
    else {
        Remove-Item -Recurse -Force $RunDir -ErrorAction SilentlyContinue
    }
}

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. "$PSScriptRoot/../redis-common.ps1"
. "$PSScriptRoot/../sample-build-common.ps1"

$ScriptDir = $PSScriptRoot
$CppRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$SampleBuild = Resolve-ZlinkCppSampleBuild -SampleDir $ScriptDir -CppRoot $CppRoot -RequiredBinaries @(
    "sample_cpp_framework_zoneworld_zone_node",
    "sample_cpp_framework_zoneworld_gateway",
    "sample_cpp_framework_zoneworld_ops",
    "sample_cpp_framework_zoneworld_client"
)
$BuildDir = $SampleBuild.BuildDir
$BuildConfiguration = $SampleBuild.Configuration
$RunDir = Join-Path ([System.IO.Path]::GetTempPath()) "zoneworld-cpp-$PID-$([Guid]::NewGuid().ToString('N'))"
$LogDir = Join-Path $RunDir "logs"
$ConfigDir = Join-Path $RunDir "config"
New-Item -ItemType Directory -Force -Path $LogDir, $ConfigDir | Out-Null

function Find-Binary([string]$Name) {
    return Get-ZlinkCppSampleBinary -Build $SampleBuild -Name $Name
}

function Wait-Port([string]$Name, [string]$Endpoint, [int]$TimeoutSeconds = 30) {
    $value = $Endpoint -replace '^tcp://', '' -replace '^http://', ''
    $separator = $value.LastIndexOf(':')
    $hostName = $value.Substring(0, $separator)
    $port = [int]$value.Substring($separator + 1)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $client = [System.Net.Sockets.TcpClient]::new()
        try {
            $connect = $client.BeginConnect($hostName, $port, $null, $null)
            if ($connect.AsyncWaitHandle.WaitOne(200)) {
                $client.EndConnect($connect)
                return
            }
        } catch {
        } finally {
            $client.Dispose()
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for $Name at $Endpoint"
}

$Processes = New-Object System.Collections.Generic.List[System.Diagnostics.Process]
function Start-Role([string]$Name, [string]$Binary, [string[]]$Arguments) {
    $process = Start-Process -FilePath $Binary -ArgumentList $Arguments -NoNewWindow -PassThru `
        -RedirectStandardOutput (Join-Path $LogDir "$Name.stdout.log") `
        -RedirectStandardError (Join-Path $LogDir "$Name.stderr.log")
    $Processes.Add($process)
    return $process
}

function Write-RoleConfig(
    [string]$Path,
    [string]$NodeId,
    [string]$MeshEndpoint,
    [string]$StreamEndpoint,
    [string]$HttpEndpoint,
    [string]$RedisEndpoint,
    [string]$RedisKeyPrefix,
    [string]$BroadcastEndpoint
) {
    @{
        sample = @{
            zoneworld = @{
                redisEndpoint = $RedisEndpoint
                redisKeyPrefix = $RedisKeyPrefix
                nodeId = $NodeId
                meshEndpoint = $MeshEndpoint
                streamEndpoint = $StreamEndpoint
                broadcastEndpoint = $BroadcastEndpoint
                bootstrapHttpEndpoint = $HttpEndpoint
                logDir = $LogDir
                faultTickZone = if ($NodeId.StartsWith("zone-node-")) { "zone-nw" } else { $null }
                subscriberOnly = $false
                disableBots = $false
                allowEmptyZoneSet = $false
            }
        }
    } | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 -Path $Path
}

$RedisContainer = $null
$Status = 1
try {
    & cmake --build $BuildDir --config $BuildConfiguration --parallel 2 --target `
        sample_cpp_framework_zoneworld_zone_node `
        sample_cpp_framework_zoneworld_gateway `
        sample_cpp_framework_zoneworld_ops `
        sample_cpp_framework_zoneworld_client
    if ($LASTEXITCODE -ne 0) { throw "ZoneWorld sample build failed with exit code $LASTEXITCODE." }

    $ZoneNodeBin = Find-Binary "sample_cpp_framework_zoneworld_zone_node"
    $GatewayBin = Find-Binary "sample_cpp_framework_zoneworld_gateway"
    $OpsBin = Find-Binary "sample_cpp_framework_zoneworld_ops"
    $ClientBin = Find-Binary "sample_cpp_framework_zoneworld_client"

    $ports = @(Get-ZlinkSamplePorts -Count 13)
    $Node1Mesh = "tcp://127.0.0.1:$($ports[0])"
    $Node2Mesh = "tcp://127.0.0.1:$($ports[1])"
    $GatewayMesh = "tcp://127.0.0.1:$($ports[2])"
    $OpsMesh = "tcp://127.0.0.1:$($ports[3])"
    $GameStream = "tcp://127.0.0.1:$($ports[4])"
    $OpsStream = "tcp://127.0.0.1:$($ports[5])"
    $Broadcast = "tcp://127.0.0.1:$($ports[6])"
    $Node1Http = "http://127.0.0.1:$($ports[7])"
    $Node2Http = "http://127.0.0.1:$($ports[8])"
    $GatewayHttp = "http://127.0.0.1:$($ports[9])"
    $Node1Stream = "tcp://127.0.0.1:$($ports[10])"
    $Node2Stream = "tcp://127.0.0.1:$($ports[11])"
    $OpsHttp = "http://127.0.0.1:$($ports[12])"

    $redis = Start-ZlinkSampleRedis "zlink-redis-cpp-sample-zoneworld" "redis:7-alpine"
    $RedisContainer = $redis.ContainerId
    $RedisEndpoint = "tcp://$($redis.Endpoint)"
    Wait-Port "redis" $RedisEndpoint
    $KeyPrefix = "zoneworld:cpp:${PID}:$([Guid]::NewGuid().ToString('N')):"

    Write-RoleConfig (Join-Path $ConfigDir "zone-node-1.json") "zone-node-1" $Node1Mesh $Node1Stream $Node1Http $RedisEndpoint $KeyPrefix $Broadcast
    Write-RoleConfig (Join-Path $ConfigDir "zone-node-2.json") "zone-node-2" $Node2Mesh $Node2Stream $Node2Http $RedisEndpoint $KeyPrefix $Broadcast
    Write-RoleConfig (Join-Path $ConfigDir "ops.json") "ops" $OpsMesh $OpsStream $OpsHttp $RedisEndpoint $KeyPrefix $Broadcast
    Write-RoleConfig (Join-Path $ConfigDir "gateway.json") "gateway" $GatewayMesh $GameStream $GatewayHttp $RedisEndpoint $KeyPrefix $Broadcast

    [void](Start-Role "zone-node-2" $ZoneNodeBin @("--config=$(Join-Path $ConfigDir 'zone-node-2.json')"))
    Wait-Port "zone-node-2 mesh" $Node2Mesh
    [void](Start-Role "zone-node-1" $ZoneNodeBin @("--config=$(Join-Path $ConfigDir 'zone-node-1.json')"))
    Wait-Port "zone-node-1 mesh" $Node1Mesh
    [void](Start-Role "ops" $OpsBin @("--config=$(Join-Path $ConfigDir 'ops.json')"))
    Wait-Port "ops stream" $OpsStream
    [void](Start-Role "gateway" $GatewayBin @("--config=$(Join-Path $ConfigDir 'gateway.json')"))
    Wait-Port "gateway stream" $GameStream

    Start-Sleep -Seconds 2
    Invoke-WebRequest -UseBasicParsing -Method Post -ContentType "application/json" -Body "{}" `
        -Uri "$GatewayHttp/bootstrap-world" -TimeoutSec 30 | Out-Null
    Start-Sleep -Seconds 6

    $client = Start-Role "client" $ClientBin @("--game-endpoint", $GameStream, "--ops-endpoint", $OpsStream)
    if (-not $client.WaitForExit(180000)) {
        Stop-Process -Id $client.Id -Force -ErrorAction SilentlyContinue
        throw "ZoneWorld client timed out after 180 seconds."
    }
    if ($client.ExitCode -ne 0) {
        throw "ZoneWorld client failed with exit code $($client.ExitCode)."
    }
    $clientLog = Get-Content (Join-Path $LogDir "client.stdout.log") -Raw
    if ($clientLog -notmatch "scenario ZW-A1 passed") {
        throw "ZoneWorld client did not emit its first typed scenario proof."
    }

    $Status = 0
    Write-Host "C++ ZoneWorld Windows smoke result=passed"
} finally {
    for ($index = $Processes.Count - 1; $index -ge 0; $index--) {
        $process = $Processes[$index]
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
    }
    if ($RedisContainer) { Remove-ZlinkSampleRedis $RedisContainer }
    if ($Status -eq 0 -and $env:ZLINK_CPP_KEEP_RUN_DIR -ne "1") {
        Remove-Item -Recurse -Force -LiteralPath $RunDir
    } else {
        Write-Host "ZoneWorld runDir=$RunDir"
    }
}

Set-StrictMode -Version Latest
. "$PSScriptRoot/../../redis-common.ps1"
$ErrorActionPreference = "Stop"

$SampleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $SampleDir

$RunDir = Join-Path ([IO.Path]::GetTempPath()) ("zlink-shoppingmall-" + [Guid]::NewGuid().ToString("N"))
$LogDir = Join-Path $RunDir "logs"
$FlowLogDir = $LogDir
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $LogDir "*.log")
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $FlowLogDir "*.log")

$Gradle = if ($IsWindows) { Join-Path $SampleDir "../../gradlew.bat" } else { Join-Path $SampleDir "../../gradlew" }
$Processes = New-Object System.Collections.Generic.List[System.Diagnostics.Process]
$RedisContainer = $null

function Print-Logs {
    param([int]$Status)
    if ($Status -eq 0) { return }
    Get-ChildItem -Path $LogDir -Filter "*.log" -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Error "===== $($_.FullName) ====="
        Get-Content -Path $_.FullName -Tail 200 -ErrorAction SilentlyContinue | ForEach-Object { Write-Error $_ }
    }
}

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
    foreach ($childId in $children) {
        Stop-Process -Id $childId -Force -ErrorAction SilentlyContinue
    }
    if (-not $Process.HasExited) {
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
    }
}

function Cleanup {
    param([int]$Status)
    Print-Logs $Status
    for ($i = $Processes.Count - 1; $i -ge 0; $i--) {
        Stop-TrackedProcessTree -Process $Processes[$i]
    }
    if ($RedisContainer) {
        Remove-ZlinkSampleRedis $RedisContainer
    }
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $RunDir
}

function Reserve-Endpoints {
    param([int]$Count)
    $listeners = New-Object System.Collections.Generic.List[System.Net.Sockets.TcpListener]
    $endpoints = New-Object System.Collections.Generic.List[string]
    try {
        while ($endpoints.Count -lt $Count) {
            $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Parse("127.0.0.1"), 0)
            $listener.Start()
            $listeners.Add($listener)
            $endpoints.Add("127.0.0.1:$($listener.LocalEndpoint.Port)")
        }
        return $endpoints.ToArray()
    } finally {
        foreach ($listener in $listeners) {
            $listener.Stop()
        }
    }
}

function Split-Endpoint {
    param([string]$Endpoint)
    $parts = $Endpoint.Split(":")
    return @{ Host = $parts[0]; Port = [int]$parts[1] }
}

function Wait-Port {
    param([string]$HostName, [int]$Port, [int]$TimeoutSeconds = 60)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $client = [System.Net.Sockets.TcpClient]::new()
        try {
            $connect = $client.BeginConnect($HostName, $Port, $null, $null)
            if ($connect.AsyncWaitHandle.WaitOne(200)) {
                $client.EndConnect($connect)
                return
            }
        } catch {
        } finally {
            $client.Close()
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for ${HostName}:$Port"
}

function Invoke-Gradle {
    param([string[]]$Arguments)
    & $Gradle @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Gradle failed: $($Arguments -join ' ')"
    }
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

function Write-ConfigFile {
    param([string]$Name, [string[]]$Lines)
    $path = Join-Path $RunDir "$Name.properties"
    Set-Content -Path $path -Value $Lines -Encoding utf8NoBOM
    Protect-ConfigFile $path
    return $path
}

function Start-Role {
    param([string]$ScriptPath, [string]$LogName, [string]$ConfigPath)
    $logPath = Join-Path $LogDir $LogName
    $errorLogPath = Join-Path $LogDir ($LogName + ".err.log")
    $process = Start-Process -FilePath $ScriptPath -ArgumentList @("--config", $ConfigPath) -WorkingDirectory $SampleDir -NoNewWindow -RedirectStandardOutput $logPath -RedirectStandardError $errorLogPath -PassThru
    $Processes.Add($process)
}

function App-Bin {
    param([string]$Project, [string]$Script)
    $bin = Join-Path $SampleDir "$Project/build/install/$Script/bin"
    if ($IsWindows) {
        return Join-Path $bin "$Script.bat"
    }
    return Join-Path $bin $Script
}

$Status = 1
try {
    $endpoints = Reserve-Endpoints 10
    $apiAHttp = Split-Endpoint $endpoints[0]
    $apiBHttp = Split-Endpoint $endpoints[1]
    $workflowAHttp = Split-Endpoint $endpoints[2]
    $workflowBHttp = Split-Endpoint $endpoints[3]
    $workflowAChannel = Split-Endpoint $endpoints[4]
    $workflowBChannel = Split-Endpoint $endpoints[5]
    $workflowASpot = Split-Endpoint $endpoints[6]
    $workflowBSpot = Split-Endpoint $endpoints[7]
    $workflowARouter = Split-Endpoint $endpoints[8]
    $workflowBRouter = Split-Endpoint $endpoints[9]

    $redis = Start-ZlinkSampleRedis "zlink-redis-java-sample-shoppingmall"
    $RedisContainer = $redis.ContainerId
    $redisEndpoint = $redis.Endpoint
    $redis = Split-Endpoint $redisEndpoint
    Wait-Port $redis.Host $redis.Port

    $redisKeyPrefix = "shoppingmall:java:${PID}:$([Guid]::NewGuid().ToString('N')):"
    $commonConfig = @(
        "sample.logDirectory=$($LogDir.Replace('\', '/'))",
        "sample.redisEndpoint=$redisEndpoint",
        "sample.redisKeyPrefix=$redisKeyPrefix"
    )
    $workflowAConfig = Write-ConfigFile "workflow-a" ($commonConfig + @(
        "sample.instanceName=workflow-a",
        "sample.httpUrl=http://$($workflowAHttp.Host):$($workflowAHttp.Port)",
        "sample.channelEndpoint=tcp://$($workflowAChannel.Host):$($workflowAChannel.Port)",
        "sample.spotEndpoint=tcp://$($workflowASpot.Host):$($workflowASpot.Port)",
        "sample.spotRouterEndpoint=tcp://$($workflowARouter.Host):$($workflowARouter.Port)"
    ))
    $workflowBConfig = Write-ConfigFile "workflow-b" ($commonConfig + @(
        "sample.instanceName=workflow-b",
        "sample.httpUrl=http://$($workflowBHttp.Host):$($workflowBHttp.Port)",
        "sample.channelEndpoint=tcp://$($workflowBChannel.Host):$($workflowBChannel.Port)",
        "sample.spotEndpoint=tcp://$($workflowBSpot.Host):$($workflowBSpot.Port)",
        "sample.spotRouterEndpoint=tcp://$($workflowBRouter.Host):$($workflowBRouter.Port)"
    ))
    $apiAConfig = Write-ConfigFile "api-a" ($commonConfig + @(
        "sample.instanceName=api-a",
        "sample.httpUrl=http://$($apiAHttp.Host):$($apiAHttp.Port)"
    ))
    $apiBConfig = Write-ConfigFile "api-b" ($commonConfig + @(
        "sample.instanceName=api-b",
        "sample.httpUrl=http://$($apiBHttp.Host):$($apiBHttp.Port)"
    ))
    $clientConfig = Write-ConfigFile "client" @(
        "sample.apiAHttpUrl=http://$($apiAHttp.Host):$($apiAHttp.Port)",
        "sample.apiBHttpUrl=http://$($apiBHttp.Host):$($apiBHttp.Port)"
    )

    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue `
        (Join-Path $SampleDir "Server/CommerceApi/build/install/CommerceApi"), `
        (Join-Path $SampleDir "Server/OrderWorkflow/build/install/OrderWorkflow"), `
        (Join-Path $SampleDir "Client/build/install/Client")

    Invoke-Gradle @("--settings-file", "standalone.settings.gradle.kts", "--no-daemon", ":Server:CommerceApi:installDist", ":Server:OrderWorkflow:installDist", ":Client:installDist", "--quiet")

    Start-Role -ScriptPath (App-Bin "Server/OrderWorkflow" "OrderWorkflow") -LogName "workflow-a.log" -ConfigPath $workflowAConfig
    Start-Role -ScriptPath (App-Bin "Server/OrderWorkflow" "OrderWorkflow") -LogName "workflow-b.log" -ConfigPath $workflowBConfig
    Wait-Port $workflowAChannel.Host $workflowAChannel.Port
    Wait-Port $workflowBChannel.Host $workflowBChannel.Port
    Wait-Port $workflowASpot.Host $workflowASpot.Port
    Wait-Port $workflowBSpot.Host $workflowBSpot.Port
    Wait-Port $workflowARouter.Host $workflowARouter.Port
    Wait-Port $workflowBRouter.Host $workflowBRouter.Port
    Wait-Port $workflowAHttp.Host $workflowAHttp.Port
    Wait-Port $workflowBHttp.Host $workflowBHttp.Port

    Start-Role -ScriptPath (App-Bin "Server/CommerceApi" "CommerceApi") -LogName "api-a.log" -ConfigPath $apiAConfig
    Start-Role -ScriptPath (App-Bin "Server/CommerceApi" "CommerceApi") -LogName "api-b.log" -ConfigPath $apiBConfig
    Wait-Port $apiAHttp.Host $apiAHttp.Port
    Wait-Port $apiBHttp.Host $apiBHttp.Port

    Write-Host "topology=ready"
    $clientLog = Join-Path $LogDir "client.log"
    & (App-Bin "Client" "Client") --config $clientConfig *> $clientLog
    if ($LASTEXITCODE -ne 0) {
        throw "Client run failed."
    }
    Get-Content -Path $clientLog

    if (-not (Select-String -Path $clientLog -Pattern "shoppingmall-server-evidence=completed" -Quiet)) {
        throw "Server evidence marker was not found."
    }
    if (-not (Select-String -Path $clientLog -Pattern "shoppingmall=completed" -Quiet)) {
        throw "Client completion marker was not found."
    }
    if (-not (Select-String -Path (Join-Path $FlowLogDir "*.log") -Pattern "message flow" -Quiet)) {
        throw "Message flow evidence was not found."
    }
    Write-Host "shoppingmall full client/server self-check completed"
    $Status = 0
} finally {
    Cleanup $Status
}

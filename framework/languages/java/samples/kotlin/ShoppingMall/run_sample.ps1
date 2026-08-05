Set-StrictMode -Version Latest
. "$PSScriptRoot/../../redis-common.ps1"
$ErrorActionPreference = "Stop"

$SampleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $SampleDir

$LogDir = Join-Path $SampleDir "build/sample-logs"
$StoreDir = Join-Path $SampleDir "build/sample-store"
$ShoppingMallLogDir = if ($env:SHOPPINGMALL_LOG_DIR) { $env:SHOPPINGMALL_LOG_DIR } else { Join-Path $SampleDir "logs" }
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
New-Item -ItemType Directory -Force -Path $StoreDir | Out-Null
New-Item -ItemType Directory -Force -Path $ShoppingMallLogDir | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $LogDir "*.log")
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $ShoppingMallLogDir "*.log")
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $StoreDir "*")

$Gradle = if ($IsWindows) { Join-Path $SampleDir "../../gradlew.bat" } else { Join-Path $SampleDir "../../gradlew" }
$Processes = New-Object System.Collections.Generic.List[System.Diagnostics.Process]
$RedisContainerId = $null

function Print-Logs {
    param([int]$Status)
    if ($Status -eq 0) { return }
    Get-ChildItem -Path $LogDir -Filter "*.log" -ErrorAction SilentlyContinue | ForEach-Object {
        [Console]::Error.WriteLine("===== $($_.FullName) =====")
        Get-Content -Path $_.FullName -Tail 200 -ErrorAction SilentlyContinue | ForEach-Object {
            [Console]::Error.WriteLine($_)
        }
    }
}

function Cleanup {
    param([int]$Status)
    Print-Logs $Status
    for ($i = $Processes.Count - 1; $i -ge 0; $i--) {
        $process = $Processes[$i]
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
    }
    if ($RedisContainerId) {
        Remove-ZlinkSampleRedis $RedisContainerId
    }
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

function Split-Endpoint {
    param([string]$Endpoint)
    $parts = $Endpoint.Split(":")
    return @{ Host = $parts[0]; Port = [int]$parts[1] }
}

function Invoke-Gradle {
    param([string[]]$Arguments)
    & $Gradle "--no-parallel" "--max-workers=1" @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Gradle failed: $($Arguments -join ' ')"
    }
}

function Start-GradleRole {
    param([string[]]$Arguments, [string]$LogName)
    $logPath = Join-Path $LogDir $LogName
    $errorLogPath = Join-Path $LogDir ($LogName + ".err.log")
    $gradleArguments = @("--no-parallel", "--max-workers=1") + $Arguments
    $process = Start-Process -FilePath $Gradle -ArgumentList $gradleArguments -WorkingDirectory $SampleDir -NoNewWindow -RedirectStandardOutput $logPath -RedirectStandardError $errorLogPath -PassThru
    $Processes.Add($process)
}

function Get-AppBin {
    param([string]$ProjectPath, [string]$AppName)
    $scriptName = if ($IsWindows) { "$AppName.bat" } else { $AppName }
    return Join-Path $SampleDir (Join-Path $ProjectPath "build/install/$AppName/bin/$scriptName")
}

function Start-Role {
    param([string]$ProjectPath, [string]$AppName, [string[]]$Arguments, [string]$LogName)
    $logPath = Join-Path $LogDir $LogName
    $errorLogPath = Join-Path $LogDir ($LogName + ".err.log")
    $process = Start-Process -FilePath (Get-AppBin $ProjectPath $AppName) -ArgumentList $Arguments -WorkingDirectory $SampleDir -NoNewWindow -RedirectStandardOutput $logPath -RedirectStandardError $errorLogPath -PassThru
    $Processes.Add($process)
}

function Invoke-App {
    param([string]$ProjectPath, [string]$AppName, [string]$LogName)
    $logPath = Join-Path $LogDir $LogName
    $errorLogPath = Join-Path $LogDir ($LogName + ".err.log")
    $process = Start-Process -FilePath (Get-AppBin $ProjectPath $AppName) -WorkingDirectory $SampleDir -NoNewWindow -RedirectStandardOutput $logPath -RedirectStandardError $errorLogPath -Wait -PassThru
    if ($process.ExitCode -ne 0) {
        throw "$AppName failed with exit code $($process.ExitCode)"
    }
}

function Assert-LogContains {
    param([string]$Path, [string]$Pattern)
    $paths = @($Path, "$Path.err.log") | Where-Object { Test-Path $_ }
    if (-not (Select-String -Path $paths -Pattern $Pattern -SimpleMatch -Quiet)) {
        throw "Expected '$Pattern' in $Path"
    }
}

$Status = 1
$oldJavaToolOptions = $env:JAVA_TOOL_OPTIONS
$oldShoppingMallLogDir = $env:SHOPPINGMALL_LOG_DIR
try {
    $endpoints = Reserve-Endpoints 4
    $commerceA = Split-Endpoint $endpoints[0]
    $commerceB = Split-Endpoint $endpoints[1]
    $workflowA = Split-Endpoint $endpoints[2]
    $workflowB = Split-Endpoint $endpoints[3]

    $redisInstance = Start-ZlinkSampleRedis "zlink-redis-kotlin-sample-shoppingmall"
    $RedisContainerId = $redisInstance.ContainerId
    $redisEndpoint = $redisInstance.Endpoint
    $redis = Split-Endpoint $redisEndpoint
    Wait-Port $redis.Host $redis.Port

    $redisKeyPrefix = if ($env:SHOPPINGMALL_REDIS_KEY_PREFIX) { $env:SHOPPINGMALL_REDIS_KEY_PREFIX } else { "shoppingmall:kotlin:${PID}:$([Guid]::NewGuid().ToString('N')):" }

    $prefix = "zlink.samples.shoppingmall"
    $env:JAVA_TOOL_OPTIONS = "$oldJavaToolOptions -D$prefix.commerceApiAEndpoint=tcp://$($commerceA.Host):$($commerceA.Port) -D$prefix.commerceApiBEndpoint=tcp://$($commerceB.Host):$($commerceB.Port) -D$prefix.workflowAEndpoint=tcp://$($workflowA.Host):$($workflowA.Port) -D$prefix.workflowBEndpoint=tcp://$($workflowB.Host):$($workflowB.Port) -D$prefix.redisEndpoint=$redisEndpoint -D$prefix.redisKeyPrefix=$redisKeyPrefix -D$prefix.storeDir=$StoreDir"
    $env:SHOPPINGMALL_LOG_DIR = $ShoppingMallLogDir

    Push-Location "../../.."
    try {
        & ./gradlew --no-daemon --no-parallel --max-workers=1 :zlink-framework-core:jar :zlink-framework-spring-boot-starter:jar :zlink-framework-kotlin:jar :zlink-framework-locations-redis:jar --quiet
        if ($LASTEXITCODE -ne 0) { throw "Framework jar build failed" }
    } finally {
        Pop-Location
    }

    Invoke-Gradle @("--settings-file", "standalone.settings.gradle.kts", "--no-daemon", ":Server:OrderWorkflow:installDist", ":Server:CommerceApi:installDist", ":Client:installDist", "--quiet")

    Start-Role -ProjectPath "Server/OrderWorkflow" -AppName "OrderWorkflow" -Arguments @("--instance", "workflow-a") -LogName "workflow-a.log"
    Wait-Port $workflowA.Host $workflowA.Port

    Start-Role -ProjectPath "Server/OrderWorkflow" -AppName "OrderWorkflow" -Arguments @("--instance", "workflow-b") -LogName "workflow-b.log"
    Wait-Port $workflowB.Host $workflowB.Port

    Start-Role -ProjectPath "Server/CommerceApi" -AppName "CommerceApi" -Arguments @("--instance", "api-a") -LogName "api-a.log"
    Wait-Port $commerceA.Host $commerceA.Port

    Start-Role -ProjectPath "Server/CommerceApi" -AppName "CommerceApi" -Arguments @("--instance", "api-b") -LogName "api-b.log"
    Wait-Port $commerceB.Host $commerceB.Port

    Invoke-App -ProjectPath "Client" -AppName "Client" -LogName "client.log"
    Assert-LogContains -Path (Join-Path $LogDir "workflow-a.log") -Pattern "shoppingmall order: started"
    Assert-LogContains -Path (Join-Path $LogDir "workflow-b.log") -Pattern "shoppingmall order: started"
    Assert-LogContains -Path (Join-Path $LogDir "api-a.log") -Pattern "shoppingmall evidence:"
    if (-not (Select-String -Path (Join-Path $ShoppingMallLogDir "*.log") -Pattern "message flow" -SimpleMatch -Quiet -ErrorAction SilentlyContinue)) {
        throw "Expected 'message flow' in $ShoppingMallLogDir"
    }
    Write-Output "shoppingmall-server-evidence=completed"
    $Status = 0
} finally {
    Cleanup $Status
    $env:JAVA_TOOL_OPTIONS = $oldJavaToolOptions
    $env:SHOPPINGMALL_LOG_DIR = $oldShoppingMallLogDir
}

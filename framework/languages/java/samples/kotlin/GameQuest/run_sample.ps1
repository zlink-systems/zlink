Set-StrictMode -Version Latest
. "$PSScriptRoot/../../redis-common.ps1"
$ErrorActionPreference = "Stop"

$SampleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $SampleDir

$RunDir = Join-Path ([System.IO.Path]::GetTempPath()) "gamequest-kotlin-$PID-$([Guid]::NewGuid().ToString('N'))"
$LogDir = Join-Path $RunDir "logs"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

$Gradle = if ($IsWindows) { Join-Path $SampleDir "../../gradlew.bat" } else { Join-Path $SampleDir "../../gradlew" }
$Processes = New-Object System.Collections.Generic.List[System.Diagnostics.Process]
$RedisContainerId = $null

function Print-Logs {
    param([int]$Status)
    if ($Status -eq 0) { return }
    Get-ChildItem -Path $LogDir -Filter "*.log" -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Error "===== $($_.FullName) ====="
        Get-Content -Path $_.FullName -Tail 200 -ErrorAction SilentlyContinue | ForEach-Object { Write-Error $_ }
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
    Remove-Item -Recurse -Force $RunDir -ErrorAction SilentlyContinue
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

function Wait-HttpHealth {
    param([string]$BaseUrl, [int]$TimeoutSeconds = 60)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            Invoke-WebRequest -Method Get -Uri "$BaseUrl/health" -UseBasicParsing -TimeoutSec 2 | Out-Null
            return
        } catch {
            Start-Sleep -Milliseconds 100
        }
    }
    throw "Timed out waiting for health at $BaseUrl"
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

function Start-Role {
    param([string]$Project, [string]$ScriptName, [string]$LogName, [string]$ConfigPath)
    $bin = Join-Path $SampleDir "$Project/build/install/$ScriptName/bin/$ScriptName"
    if ($IsWindows) {
        $bin = "$bin.bat"
    }
    $logPath = Join-Path $LogDir $LogName
    $errorLogPath = Join-Path $LogDir ($LogName + ".err.log")
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $bin
    $startInfo.WorkingDirectory = $SampleDir
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.UseShellExecute = $false
    $startInfo.ArgumentList.Add("--config")
    $startInfo.ArgumentList.Add($ConfigPath)
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    $process.Start() | Out-Null
    $process.BeginOutputReadLine()
    $process.BeginErrorReadLine()
    Register-ObjectEvent -InputObject $process -EventName OutputDataReceived -Action {
        if ($EventArgs.Data) { Add-Content -Path $Event.MessageData.Out -Value $EventArgs.Data }
    } -MessageData @{ Out = $logPath } | Out-Null
    Register-ObjectEvent -InputObject $process -EventName ErrorDataReceived -Action {
        if ($EventArgs.Data) { Add-Content -Path $Event.MessageData.Err -Value $EventArgs.Data }
    } -MessageData @{ Err = $errorLogPath } | Out-Null
    $Processes.Add($process)
}

function Protect-ConfigFile {
    param([string]$Path)
    if ($IsWindows) {
        & icacls $Path /inheritance:r /grant:r "$env:USERNAME`:(R,W)" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Could not restrict config ACL: $Path" }
    } else {
        & chmod 600 $Path
        if ($LASTEXITCODE -ne 0) { throw "Could not restrict config mode: $Path" }
    }
}

$Status = 1
try {
    $endpoints = Reserve-Endpoints 8
    $apiAStream = Split-Endpoint $endpoints[0]
    $apiBStream = Split-Endpoint $endpoints[1]
    $apiAHttp = Split-Endpoint $endpoints[2]
    $apiBHttp = Split-Endpoint $endpoints[3]
    $missionARoute = Split-Endpoint $endpoints[4]
    $missionBRoute = Split-Endpoint $endpoints[5]
    $missionAHttp = Split-Endpoint $endpoints[6]
    $missionBHttp = Split-Endpoint $endpoints[7]

    $redisInstance = Start-ZlinkSampleRedis "zlink-redis-kotlin-sample-gamequest"
    $RedisContainerId = $redisInstance.ContainerId
    $redisEndpoint = $redisInstance.Endpoint
    $redis = Split-Endpoint $redisEndpoint
    Wait-Port $redis.Host $redis.Port

    $redisKeyPrefix = "gamequest:kotlin:${PID}:$([Guid]::NewGuid().ToString('N')):"
    $apiAStreamEndpoint = "tcp://$($apiAStream.Host):$($apiAStream.Port)"
    $apiBStreamEndpoint = "tcp://$($apiBStream.Host):$($apiBStream.Port)"
    $apiAHttpEndpoint = "http://$($apiAHttp.Host):$($apiAHttp.Port)"
    $apiBHttpEndpoint = "http://$($apiBHttp.Host):$($apiBHttp.Port)"
    $missionAChannelEndpoint = "tcp://$($missionARoute.Host):$($missionARoute.Port)"
    $missionBChannelEndpoint = "tcp://$($missionBRoute.Host):$($missionBRoute.Port)"
    $missionAHttpEndpoint = "http://$($missionAHttp.Host):$($missionAHttp.Port)"
    $missionBHttpEndpoint = "http://$($missionBHttp.Host):$($missionBHttp.Port)"

    $missionAConfig = Join-Path $RunDir "mission-a.properties"
    $missionBConfig = Join-Path $RunDir "mission-b.properties"
    $apiAConfig = Join-Path $RunDir "api-a.properties"
    $apiBConfig = Join-Path $RunDir "api-b.properties"
    $clientConfig = Join-Path $RunDir "client.properties"
    @("sample.instanceName=mission-a", "sample.logDirectory=$LogDir", "sample.channelEndpoint=$missionAChannelEndpoint", "sample.httpEndpoint=$missionAHttpEndpoint", "sample.redisEndpoint=$redisEndpoint", "sample.redisKeyPrefix=$redisKeyPrefix") | Set-Content $missionAConfig -Encoding UTF8
    @("sample.instanceName=mission-b", "sample.logDirectory=$LogDir", "sample.channelEndpoint=$missionBChannelEndpoint", "sample.httpEndpoint=$missionBHttpEndpoint", "sample.redisEndpoint=$redisEndpoint", "sample.redisKeyPrefix=$redisKeyPrefix") | Set-Content $missionBConfig -Encoding UTF8
    @("sample.instanceName=api-a", "sample.logDirectory=$LogDir", "sample.streamEndpoint=$apiAStreamEndpoint", "sample.httpEndpoint=$apiAHttpEndpoint", "sample.missionAChannelEndpoint=$missionAChannelEndpoint", "sample.missionBChannelEndpoint=$missionBChannelEndpoint", "sample.redisEndpoint=$redisEndpoint", "sample.redisKeyPrefix=$redisKeyPrefix") | Set-Content $apiAConfig -Encoding UTF8
    @("sample.instanceName=api-b", "sample.logDirectory=$LogDir", "sample.streamEndpoint=$apiBStreamEndpoint", "sample.httpEndpoint=$apiBHttpEndpoint", "sample.missionAChannelEndpoint=$missionAChannelEndpoint", "sample.missionBChannelEndpoint=$missionBChannelEndpoint", "sample.redisEndpoint=$redisEndpoint", "sample.redisKeyPrefix=$redisKeyPrefix") | Set-Content $apiBConfig -Encoding UTF8
    @("sample.apiAStreamEndpoint=$apiAStreamEndpoint", "sample.apiBStreamEndpoint=$apiBStreamEndpoint", "sample.apiAHttpEndpoint=$apiAHttpEndpoint", "sample.apiBHttpEndpoint=$apiBHttpEndpoint", "sample.missionAHttpEndpoint=$missionAHttpEndpoint", "sample.missionBHttpEndpoint=$missionBHttpEndpoint") | Set-Content $clientConfig -Encoding UTF8
    @($missionAConfig, $missionBConfig, $apiAConfig, $apiBConfig, $clientConfig) | ForEach-Object { Protect-ConfigFile $_ }

    Push-Location "../../.."
    try {
        & ./gradlew --no-daemon --no-parallel --max-workers=1 :zlink-framework-core:jar :zlink-framework-kotlin:jar :zlink-framework-spring-boot-starter:jar :zlink-framework-locations-redis:jar :zlink-stream-connector:jar --quiet
        if ($LASTEXITCODE -ne 0) { throw "Framework jar build failed" }
    } finally {
        Pop-Location
    }

    Invoke-Gradle @("--settings-file", "standalone.settings.gradle.kts", "--no-daemon", ":Server:GameApi:installDist", ":Server:QuestMission:installDist", ":Client:installDist", "--quiet")

    Start-Role -Project "Server/QuestMission" -ScriptName "QuestMission" -LogName "mission-a.log" -ConfigPath $missionAConfig
    Start-Role -Project "Server/QuestMission" -ScriptName "QuestMission" -LogName "mission-b.log" -ConfigPath $missionBConfig
    Wait-Port $missionARoute.Host $missionARoute.Port
    Wait-Port $missionBRoute.Host $missionBRoute.Port
    Wait-HttpHealth "http://$($missionAHttp.Host):$($missionAHttp.Port)"
    Wait-HttpHealth "http://$($missionBHttp.Host):$($missionBHttp.Port)"

    Start-Role -Project "Server/GameApi" -ScriptName "GameApi" -LogName "api-a.log" -ConfigPath $apiAConfig
    Start-Role -Project "Server/GameApi" -ScriptName "GameApi" -LogName "api-b.log" -ConfigPath $apiBConfig
    Wait-Port $apiAStream.Host $apiAStream.Port
    Wait-Port $apiBStream.Host $apiBStream.Port
    Wait-HttpHealth "http://$($apiAHttp.Host):$($apiAHttp.Port)"
    Wait-HttpHealth "http://$($apiBHttp.Host):$($apiBHttp.Port)"

    Write-Host "topology=ready"
    $clientBin = Join-Path $SampleDir "Client/build/install/Client/bin/Client"
    if ($IsWindows) {
        $clientBin = "$clientBin.bat"
    }
    & $clientBin --config $clientConfig | Tee-Object -FilePath (Join-Path $LogDir "client.log")
    if ($LASTEXITCODE -ne 0) { throw "Client failed" }

    Select-String -Path (Join-Path $LogDir "client.log") -Pattern "gamequest-server-evidence=completed" | Out-Null
    Select-String -Path (Join-Path $LogDir "client.log") -Pattern "gamequest=completed" | Out-Null
    Write-Host "gamequest kotlin full client/server self-check completed"
    $Status = 0
} finally {
    Cleanup $Status
}

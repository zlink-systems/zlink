Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:SampleProcesses = @()

function Invoke-SampleDockerCommand {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [int]$TimeoutSeconds = 10,
        [switch]$AllowFailure
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = "docker"
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in $Arguments) {
        $startInfo.ArgumentList.Add($argument)
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) {
            throw "Failed to start docker command."
        }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $process.Kill($true)
            throw "Docker command timed out after $TimeoutSeconds seconds: docker $($Arguments -join ' ')"
        }
        $result = [pscustomobject]@{
            ExitCode = $process.ExitCode
            StdOut = $stdout.GetAwaiter().GetResult().Trim()
            StdErr = $stderr.GetAwaiter().GetResult().Trim()
        }
        if (-not $AllowFailure -and $result.ExitCode -ne 0) {
            throw "Docker command failed ($($result.ExitCode)): docker $($Arguments -join ' ')`n$($result.StdErr)"
        }
        return $result
    }
    finally {
        $process.Dispose()
    }
}

function Remove-SampleRedisContainer {
    param([Parameter(Mandatory = $true)][string]$ContainerId)

    if (-not $ContainerId) { return }
    Invoke-SampleDockerCommand -Arguments @("rm", "-fv", $ContainerId) -AllowFailure | Out-Null
}

function Start-SampleRedisContainer {
    param(
        [Parameter(Mandatory = $true)][string]$Scope,
        [string]$Image = "redis:7.2-alpine"
    )

    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
        throw "Docker is required to run this sample."
    }

    $name = "$Scope-$PID-$([Guid]::NewGuid().ToString('N'))"
    $created = Invoke-SampleDockerCommand -Arguments @(
        "create", "--name", $name, "--tmpfs", "/data",
        "-p", "127.0.0.1::6379", $Image)
    $containerId = $created.StdOut
    if ($containerId -notmatch '^[0-9a-f]{12,64}$') {
        throw "Docker create did not return a Redis container id for $name."
    }

    try {
        Invoke-SampleDockerCommand -Arguments @("start", $containerId) | Out-Null
        $running = Invoke-SampleDockerCommand -Arguments @(
            "inspect", "-f", "{{.State.Running}}", $containerId)
        if ($running.StdOut -ne "true") {
            throw "Redis container $name did not enter the running state."
        }
        $port = Invoke-SampleDockerCommand -Arguments @(
            "inspect", "-f", "{{(index (index .NetworkSettings.Ports `"6379/tcp`") 0).HostPort}}", $containerId)
        if ($port.StdOut -notmatch '^\d+$') {
            throw "Docker inspect did not return a Redis host port for $name."
        }
        return [pscustomobject]@{
            ContainerId = $containerId
            Endpoint = "127.0.0.1:$($port.StdOut)"
        }
    }
    catch {
        Remove-SampleRedisContainer $containerId
        throw
    }
}

function New-SampleRunDirectory {
    param([Parameter(Mandatory = $true)][string]$Name)

    $path = Join-Path ([System.IO.Path]::GetTempPath()) "$Name-$([System.Guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Force -Path $path | Out-Null
    if ($IsWindows) {
        $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
        $security = [System.Security.AccessControl.DirectorySecurity]::new()
        $security.SetAccessRuleProtection($true, $false)
        $inheritance = [System.Security.AccessControl.InheritanceFlags]::ContainerInherit -bor
            [System.Security.AccessControl.InheritanceFlags]::ObjectInherit
        $rule = [System.Security.AccessControl.FileSystemAccessRule]::new(
            $identity,
            [System.Security.AccessControl.FileSystemRights]::FullControl,
            $inheritance,
            [System.Security.AccessControl.PropagationFlags]::None,
            [System.Security.AccessControl.AccessControlType]::Allow)
        $security.AddAccessRule($rule)
        [System.IO.Directory]::SetAccessControl($path, $security)
    }
    else {
        [System.IO.File]::SetUnixFileMode(
            $path,
            [System.IO.UnixFileMode]::UserRead -bor
            [System.IO.UnixFileMode]::UserWrite -bor
            [System.IO.UnixFileMode]::UserExecute)
    }
    return $path
}

function Remove-SampleConfigurationFiles {
    param([Parameter(Mandatory = $true)][string]$RunDirectory)

    Get-ChildItem -Path $RunDirectory -Filter "*.json" -File -Recurse -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
}

function New-SamplePorts {
    param(
        [Parameter(Mandatory = $true)][int]$Count,
        [int]$BasePort = 0
    )

    if ($BasePort -gt 0) {
        $ports = @()
        for ($i = 1; $i -le $Count; $i++) {
            $ports += ($BasePort + $i)
        }
        return $ports
    }

    $ports = @()
    $listeners = @()
    try {
        for ($i = 0; $i -lt $Count; $i++) {
            $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
            $listener.Start()
            $listeners += $listener
            $ports += $listener.LocalEndpoint.Port
        }
    }
    finally {
        foreach ($listener in $listeners) {
            $listener.Stop()
        }
    }

    return $ports
}

function Get-SampleEndpointHost {
    param([Parameter(Mandatory = $true)][string]$Endpoint)
    return ([System.Uri]$Endpoint).Host
}

function Get-SampleEndpointPort {
    param([Parameter(Mandatory = $true)][string]$Endpoint)
    return ([System.Uri]$Endpoint).Port
}

function Wait-SampleTcpEndpoint {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Endpoint,
        [int]$Attempts = 100
    )

    $hostName = Get-SampleEndpointHost $Endpoint
    $port = Get-SampleEndpointPort $Endpoint
    for ($i = 0; $i -lt $Attempts; $i++) {
        $client = [System.Net.Sockets.TcpClient]::new()
        try {
            $connect = $client.BeginConnect($hostName, $port, $null, $null)
            if ($connect.AsyncWaitHandle.WaitOne(100)) {
                $client.EndConnect($connect)
                return
            }
        }
        catch {
        }
        finally {
            $client.Dispose()
        }

        Start-Sleep -Milliseconds 100
    }

    throw "Timed out waiting for $Name at $Endpoint"
}

function Wait-SampleHttpHealth {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Endpoint,
        [int]$Attempts = 100
    )

    for ($i = 0; $i -lt $Attempts; $i++) {
        try {
            Invoke-WebRequest -Uri "$Endpoint/health" -UseBasicParsing -TimeoutSec 2 | Out-Null
            return
        }
        catch {
            Start-Sleep -Milliseconds 100
        }
    }

    throw "Timed out waiting for $Name at $Endpoint"
}

function Invoke-SampleDotnetBuild {
    param([Parameter(Mandatory = $true)][string]$Project)

    & dotnet build $Project --maxcpucount:1
    if ($LASTEXITCODE -ne 0) {
        throw "dotnet build failed for $Project"
    }
}

function Start-SampleDotnetAssembly {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Project,
        [Parameter(Mandatory = $true)][string]$LogDirectory,
        [string[]]$Arguments = @()
    )

    $projectPath = Resolve-Path $Project
    $projectDirectory = Split-Path -Parent $projectPath
    $projectName = [System.IO.Path]::GetFileNameWithoutExtension($projectPath)
    $assembly = Join-Path $projectDirectory "bin/Debug/net8.0/$projectName.dll"
    $argumentList = @($assembly) + $Arguments
    $process = Start-Process -FilePath "dotnet" `
        -ArgumentList $argumentList `
        -RedirectStandardOutput (Join-Path $LogDirectory "$Name.out.log") `
        -RedirectStandardError (Join-Path $LogDirectory "$Name.err.log") `
        -PassThru
    $script:SampleProcesses += $process
    return $process
}

function Invoke-SampleDotnetRun {
    param(
        [Parameter(Mandatory = $true)][string]$Project,
        [string[]]$Arguments = @()
    )

    & dotnet run --no-build --project $Project -- $Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "dotnet run failed for $Project"
    }
}

function Stop-SampleProcesses {
    for ($i = $script:SampleProcesses.Count - 1; $i -ge 0; $i--) {
        $process = $script:SampleProcesses[$i]
        try {
            if (-not $process.HasExited) {
                $process.CloseMainWindow() | Out-Null
                Start-Sleep -Milliseconds 100
            }
        }
        catch {
        }
    }

    for ($i = 0; $i -lt 20; $i++) {
        $alive = @($script:SampleProcesses | Where-Object { -not $_.HasExited })
        if ($alive.Count -eq 0) {
            break
        }
        Start-Sleep -Milliseconds 100
    }

    foreach ($process in $script:SampleProcesses) {
        try {
            if (-not $process.HasExited) {
                $process.Kill($true)
            }
            $process.WaitForExit()
        }
        catch {
        }
        finally {
            $process.Dispose()
        }
    }
}

function Assert-SampleLogContains {
    param(
        [Parameter(Mandatory = $true)][string]$LogDirectory,
        [Parameter(Mandatory = $true)][string]$Pattern
    )

    $match = Get-ChildItem -Path $LogDirectory -Filter "*.log" |
        Select-String -SimpleMatch $Pattern |
        Select-Object -First 1
    if ($null -eq $match) {
        throw "Log evidence was not found: $Pattern"
    }
}

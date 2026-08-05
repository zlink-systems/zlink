Set-StrictMode -Version Latest

function Invoke-ZlinkDockerCommand {
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
    if (-not $process.Start()) {
        throw "Failed to start Docker: docker $($Arguments -join ' ')"
    }
    try {
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $process.Kill($true)
            throw "Docker command timed out after ${TimeoutSeconds}s: docker $($Arguments -join ' ')"
        }
        $stdout = $process.StandardOutput.ReadToEnd().Trim()
        $stderr = $process.StandardError.ReadToEnd().Trim()
        if ($process.ExitCode -ne 0 -and -not $AllowFailure) {
            throw "Docker command failed (exit=$($process.ExitCode)): docker $($Arguments -join ' ')`n$stderr"
        }
        return [PSCustomObject]@{
            ExitCode = $process.ExitCode
            Output = $stdout
            ErrorOutput = $stderr
        }
    } finally {
        $process.Dispose()
    }
}

function Wait-ZlinkSampleRedisReady {
    param(
        [Parameter(Mandatory = $true)][string]$ContainerId,
        [int]$TimeoutSeconds = 30
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $ping = Invoke-ZlinkDockerCommand -Arguments @(
            "exec", $ContainerId, "redis-cli", "ping"
        ) -TimeoutSeconds 2 -AllowFailure
        if ($ping.ExitCode -eq 0 -and $ping.Output -eq "PONG") {
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for the dedicated Redis container: $ContainerId"
}

function Start-ZlinkSampleRedis {
    param(
        [Parameter(Mandatory = $true)][string]$Scope,
        [string]$Image = "redis:7.2-alpine"
    )

    $name = "$Scope-$PID-$([Guid]::NewGuid().ToString('N'))"
    $created = Invoke-ZlinkDockerCommand -Arguments @(
        "create", "--name", $name, "--tmpfs", "/data", "-p", "127.0.0.1::6379", $Image
    )
    $containerId = $created.Output
    if (-not $containerId) {
        throw "Failed to create the dedicated Redis container: $name"
    }

    try {
        Invoke-ZlinkDockerCommand -Arguments @("start", $containerId) | Out-Null
        $running = (Invoke-ZlinkDockerCommand -Arguments @(
            "inspect", "-f", "{{.State.Running}}", $containerId
        )).Output
        $hostPort = (Invoke-ZlinkDockerCommand -Arguments @(
            "inspect", "-f", '{{(index (index .NetworkSettings.Ports "6379/tcp") 0).HostPort}}', $containerId
        )).Output
        if ($running -ne "true" -or -not $hostPort) {
            throw "Failed to inspect the dedicated Redis container: $name"
        }
        Wait-ZlinkSampleRedisReady -ContainerId $containerId
        return [PSCustomObject]@{
            ContainerId = $containerId
            Endpoint = "127.0.0.1:$hostPort"
        }
    } catch {
        Invoke-ZlinkDockerCommand -Arguments @("rm", "-fv", $containerId) -AllowFailure | Out-Null
        throw
    }
}

function Remove-ZlinkSampleRedis {
    param([string]$ContainerId)
    if ($ContainerId) {
        Invoke-ZlinkDockerCommand -Arguments @("rm", "-fv", $ContainerId) -AllowFailure | Out-Null
    }
}

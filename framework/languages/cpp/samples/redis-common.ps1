Set-StrictMode -Version Latest

$script:ZlinkCppSampleRedisPortMin = 20000
$script:ZlinkCppSampleRedisPortMax = 20099
$script:ZlinkCppSampleAppPortMin = 20100
$script:ZlinkCppSampleAppPortMax = 21999

function Test-ZlinkSampleTcpPort {
    param([Parameter(Mandatory = $true)][int]$Port)
    $listener = [System.Net.Sockets.TcpListener]::new(
        [System.Net.IPAddress]::Parse("127.0.0.1"), $Port)
    $listener.Server.ExclusiveAddressUse = $true
    try {
        $listener.Start()
        return $true
    } catch {
        return $false
    } finally {
        $listener.Stop()
    }
}

function Get-ZlinkSamplePorts {
    param(
        [Parameter(Mandatory = $true)][int]$Count,
        [switch]$Paired
    )
    if ($Count -le 0) { throw "Port count must be positive." }

    $first = $script:ZlinkCppSampleAppPortMin
    $last = if ($Paired) { 20999 } else { $script:ZlinkCppSampleAppPortMax }
    $offset = if ($Paired) { 1000 } else { 0 }
    $candidates = @($first..$last | Get-Random -Count ($last - $first + 1))
    $listeners = New-Object System.Collections.Generic.List[System.Net.Sockets.TcpListener]
    $selected = New-Object System.Collections.Generic.List[int]
    $used = [System.Collections.Generic.HashSet[int]]::new()
    try {
        foreach ($candidate in $candidates) {
            $ports = if ($offset) { @($candidate, $candidate + $offset) } else { @($candidate) }
            if (@($ports | Where-Object {
                $_ -gt $script:ZlinkCppSampleAppPortMax -or $used.Contains($_)
            }).Count) {
                continue
            }
            $current = New-Object System.Collections.Generic.List[System.Net.Sockets.TcpListener]
            $available = $true
            foreach ($port in $ports) {
                $listener = [System.Net.Sockets.TcpListener]::new(
                    [System.Net.IPAddress]::Parse("127.0.0.1"), $port)
                $listener.Server.ExclusiveAddressUse = $true
                try {
                    $listener.Start()
                    $current.Add($listener)
                } catch {
                    $listener.Stop()
                    $available = $false
                    break
                }
            }
            if (-not $available) {
                foreach ($listener in $current) { $listener.Stop() }
                continue
            }
            foreach ($listener in $current) { $listeners.Add($listener) }
            foreach ($port in $ports) { [void]$used.Add($port) }
            $selected.Add($candidate)
            if ($selected.Count -eq $Count) { return $selected.ToArray() }
        }
        throw "Only $($selected.Count) of $Count requested ports are available in $first-$last."
    } finally {
        foreach ($listener in $listeners) { $listener.Stop() }
    }
}

function Invoke-ZlinkSampleDockerCommand {
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

function Remove-ZlinkSampleRedisAttempt {
    param(
        [AllowEmptyString()][string]$ContainerId,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $exactId = $ContainerId
    if ($exactId -notmatch '^[0-9a-f]{12,64}$') {
        $lookup = Invoke-ZlinkSampleDockerCommand -Arguments @(
            "inspect", "--type", "container", "-f", "{{.Id}}", $Name
        ) -TimeoutSeconds 5 -AllowFailure
        if ($lookup.ExitCode -eq 0) {
            $exactId = $lookup.Output.Trim()
        }
    }
    if ($exactId -match '^[0-9a-f]{12,64}$') {
        Invoke-ZlinkSampleDockerCommand -Arguments @("rm", "-fv", $exactId) `
            -TimeoutSeconds 10 -AllowFailure | Out-Null
    }
}

function Start-ZlinkSampleRedis {
    param(
        [Parameter(Mandatory = $true)][string]$Scope,
        [string]$Image = "redis:7.2-alpine"
    )
    $rangeSize = $script:ZlinkCppSampleRedisPortMax - $script:ZlinkCppSampleRedisPortMin + 1
    $startOffset = Get-Random -Minimum 0 -Maximum $rangeSize
    for ($offset = 0; $offset -lt $rangeSize; $offset++) {
        $port = $script:ZlinkCppSampleRedisPortMin + (($startOffset + $offset) % $rangeSize)
        if (-not (Test-ZlinkSampleTcpPort -Port $port)) {
            continue
        }

        $name = "$Scope-$PID-$([Guid]::NewGuid().ToString('N'))-$port"
        try {
            $created = Invoke-ZlinkSampleDockerCommand -Arguments @(
                "create", "--name", $name, "--tmpfs", "/data", "-p",
                "127.0.0.1:${port}:6379", $Image
            ) -AllowFailure
        } catch {
            Remove-ZlinkSampleRedisAttempt -ContainerId "" -Name $name
            throw
        }
        if ($created.ExitCode -ne 0) {
            $createFailure = "$($created.Output)`n$($created.ErrorOutput)"
            Remove-ZlinkSampleRedisAttempt -ContainerId $created.Output -Name $name
            if ($createFailure -match
                "address already in use|port is already allocated|failed to bind host port") {
                continue
            }
            throw "Failed to create the dedicated Redis container: $name`n$createFailure"
        }
        $containerId = $created.Output.Trim()
        if ($containerId -notmatch '^[0-9a-f]{12,64}$') {
            Remove-ZlinkSampleRedisAttempt -ContainerId $containerId -Name $name
            throw "Failed to create the dedicated Redis container: $name"
        }

        try {
            $started = Invoke-ZlinkSampleDockerCommand -Arguments @(
                "start", $containerId
            ) -AllowFailure
        } catch {
            Remove-ZlinkSampleRedisAttempt -ContainerId $containerId -Name $name
            throw
        }
        if ($started.ExitCode -ne 0) {
            Remove-ZlinkSampleRedisAttempt -ContainerId $containerId -Name $name
            $startFailure = "$($started.Output)`n$($started.ErrorOutput)"
            if ($startFailure -match
                "address already in use|port is already allocated|failed to bind host port") {
                continue
            }
            throw "Failed to start the dedicated Redis container: $name`n$startFailure"
        }

        try {
            $running = (Invoke-ZlinkSampleDockerCommand -Arguments @(
                "inspect", "-f", "{{.State.Running}}", $containerId
            )).Output
            $hostPort = (Invoke-ZlinkSampleDockerCommand -Arguments @(
                "inspect", "-f", '{{(index (index .NetworkSettings.Ports "6379/tcp") 0).HostPort}}', $containerId
            )).Output
            if ($running -ne "true" -or $hostPort -ne "$port") {
                throw "Failed to inspect the dedicated Redis container: $name"
            }
            return [PSCustomObject]@{
                ContainerId = $containerId
                Endpoint = "127.0.0.1:$hostPort"
            }
        } catch {
            Remove-ZlinkSampleRedisAttempt -ContainerId $containerId -Name $name
            throw
        }
    }

    throw "No bindable Redis host port remained in " +
        "$($script:ZlinkCppSampleRedisPortMin)-$($script:ZlinkCppSampleRedisPortMax) for $Scope."
}

function Remove-ZlinkSampleRedis {
    param([string]$ContainerId)

    if ($ContainerId -match '^[0-9a-f]{12,64}$') {
        Invoke-ZlinkSampleDockerCommand -Arguments @("rm", "-fv", $ContainerId) `
            -TimeoutSeconds 10 -AllowFailure | Out-Null
    }
}

Set-StrictMode -Version Latest

if (-not (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue)) {
    $IsWindows = $env:OS -eq "Windows_NT"
}

function Set-ZlinkSampleUtf8File {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][string[]]$Value
    )

    [System.IO.File]::WriteAllText(
        $Path,
        ($Value -join [System.Environment]::NewLine),
        [System.Text.UTF8Encoding]::new($false))
}

function ConvertTo-ZlinkSampleProcessArgument {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value)

    if ($Value -notmatch '[\s"]') {
        return $Value
    }
    return '"' + [regex]::Replace($Value, '(\\*)"', '$1$1\"') + '"'
}

function Optimize-ZlinkSampleWindowsLaunchers {
    param([Parameter(Mandatory = $true)][string]$Root)

    if (-not $IsWindows) {
        return
    }
    Get-ChildItem -Path $Root -Filter "*.bat" -Recurse -File |
        Where-Object { $_.FullName -match '[\\/]build[\\/]install[\\/][^\\/]+[\\/]bin[\\/]' } |
        ForEach-Object {
            $content = [System.IO.File]::ReadAllText($_.FullName)
            $optimized = [regex]::Replace(
                $content,
                '(?m)^set CLASSPATH=.*$',
                'set CLASSPATH=%APP_HOME%\lib\*')
            if ($optimized -ne $content) {
                [System.IO.File]::WriteAllText(
                    $_.FullName,
                    $optimized,
                    [System.Text.UTF8Encoding]::new($false))
            }
        }
}

function Get-ZlinkSamplePortPool {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet("Java", "Kotlin")]
        [string]$Language
    )

    if ($Language -eq "Java") {
        return [PSCustomObject]@{
            RedisMinimum = 24000
            RedisMaximum = 24099
            ApplicationMinimum = 24100
            ApplicationMaximum = 25999
        }
    }
    return [PSCustomObject]@{
        RedisMinimum = 26000
        RedisMaximum = 26099
        ApplicationMinimum = 26100
        ApplicationMaximum = 27999
    }
}

function Get-ZlinkSampleApplicationPorts {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet("Java", "Kotlin")]
        [string]$Language,
        [Parameter(Mandatory = $true)][int]$Count
    )

    $pool = Get-ZlinkSamplePortPool -Language $Language
    $rangeSize = $pool.ApplicationMaximum - $pool.ApplicationMinimum + 1
    if ($Count -lt 1 -or $Count -gt $rangeSize) {
        throw "Invalid $Language application port count: $Count"
    }

    $listeners = [System.Collections.Generic.List[System.Net.Sockets.TcpListener]]::new()
    $ports = [System.Collections.Generic.List[int]]::new()
    $startOffset = Get-Random -Minimum 0 -Maximum $rangeSize
    try {
        for ($offset = 0; $offset -lt $rangeSize -and $ports.Count -lt $Count; $offset++) {
            $port = $pool.ApplicationMinimum + (($startOffset + $offset) % $rangeSize)
            $listener = [System.Net.Sockets.TcpListener]::new(
                [System.Net.IPAddress]::Loopback,
                $port)
            $listener.Server.ExclusiveAddressUse = $true
            try {
                $listener.Start()
            } catch [System.Net.Sockets.SocketException] {
                $listener.Stop()
                continue
            }
            $listeners.Add($listener)
            $ports.Add($port)
        }
        if ($ports.Count -ne $Count) {
            throw "Unable to bind-check $Count $Language application ports in " +
                "$($pool.ApplicationMinimum)-$($pool.ApplicationMaximum)."
        }
        return $ports.ToArray()
    } finally {
        foreach ($listener in $listeners) {
            $listener.Stop()
        }
    }
}

function Get-ZlinkSampleApplicationEndpoints {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet("Java", "Kotlin")]
        [string]$Language,
        [Parameter(Mandatory = $true)][int]$Count
    )

    return @(
        Get-ZlinkSampleApplicationPorts -Language $Language -Count $Count |
            ForEach-Object { "127.0.0.1:$_" }
    )
}

function Test-ZlinkSampleTcpPortAvailable {
    param([Parameter(Mandatory = $true)][int]$Port)

    $listener = [System.Net.Sockets.TcpListener]::new(
        [System.Net.IPAddress]::Loopback,
        $Port)
    $listener.Server.ExclusiveAddressUse = $true
    try {
        $listener.Start()
        return $true
    } catch [System.Net.Sockets.SocketException] {
        return $false
    } finally {
        $listener.Stop()
    }
}

function Invoke-ZlinkSampleGradleBuild {
    param(
        [Parameter(Mandatory = $true)][string]$GradleExecutable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [int]$LockTimeoutSeconds = 600
    )

    $lockPath = Join-Path ([System.IO.Path]::GetTempPath()) `
        "zlink-framework-java-kotlin-sample-gradle.lock"
    $deadline = [DateTime]::UtcNow.AddSeconds($LockTimeoutSeconds)
    $lockStream = $null
    while ($null -eq $lockStream) {
        try {
            $lockStream = [System.IO.File]::Open(
                $lockPath,
                [System.IO.FileMode]::OpenOrCreate,
                [System.IO.FileAccess]::ReadWrite,
                [System.IO.FileShare]::None)
        } catch [System.IO.IOException] {
            if ([DateTime]::UtcNow -ge $deadline) {
                throw "Timed out waiting for the shared Java/Kotlin Gradle build lock: $lockPath"
            }
            Start-Sleep -Milliseconds 100
        }
    }

    try {
        $normalizedArguments = @($Arguments | ForEach-Object {
            if ($_ -eq "--settings-file") { "-c" } else { $_ }
        })
        $settingsIndex = [Array]::IndexOf($normalizedArguments, "-c")
        $temporarySettingsPath = $null
        $existingSettingsContent = $null
        if ($settingsIndex -ge 0 -and $settingsIndex + 1 -lt $normalizedArguments.Count) {
            $settingsSourcePath = Join-Path (Get-Location) $normalizedArguments[$settingsIndex + 1]
            $temporarySettingsPath = Join-Path (Get-Location) "settings.gradle.kts"
            if (Test-Path -LiteralPath $temporarySettingsPath) {
                $existingSettingsContent = Get-Content -LiteralPath $temporarySettingsPath -Raw
            }
            Copy-Item -LiteralPath $settingsSourcePath -Destination $temporarySettingsPath -Force
            $normalizedArguments = @($normalizedArguments | Where-Object { $_ -ne "-c" -and $_ -ne $normalizedArguments[$settingsIndex + 1] })
        }
        & $GradleExecutable @normalizedArguments
        if ($LASTEXITCODE -ne 0) {
            throw "Gradle build failed: $($normalizedArguments -join ' ')"
        }
        if ($Arguments -match ':installDist$') {
            Optimize-ZlinkSampleWindowsLaunchers -Root (Get-Location).Path
        }
    } finally {
        if ($temporarySettingsPath) {
            if ($null -eq $existingSettingsContent) {
                Remove-Item -LiteralPath $temporarySettingsPath -Force -ErrorAction SilentlyContinue
            } else {
                Set-Content -LiteralPath $temporarySettingsPath -Value $existingSettingsContent -NoNewline
            }
        }
        $lockStream.Dispose()
    }
}

function Invoke-ZlinkDockerCommand {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [int]$TimeoutSeconds = 10,
        [switch]$AllowFailure
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = "docker"
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.Arguments = (($Arguments | ForEach-Object {
        ConvertTo-ZlinkSampleProcessArgument $_
    }) -join " ")

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Failed to start Docker: docker $($Arguments -join ' ')"
    }
    try {
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $process.Kill()
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

function Remove-ZlinkSampleRedisAttempt {
    param(
        [AllowEmptyString()][string]$ContainerId,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $exactId = $ContainerId
    if ($exactId -notmatch '^[0-9a-f]{12,64}$') {
        $lookup = Invoke-ZlinkDockerCommand -Arguments @(
            "inspect", "--type", "container", "-f", "{{.Id}}", $Name
        ) -TimeoutSeconds 5 -AllowFailure
        if ($lookup.ExitCode -eq 0) {
            $exactId = $lookup.Output.Trim()
        }
    }
    if ($exactId -match '^[0-9a-f]{12,64}$') {
        Invoke-ZlinkDockerCommand -Arguments @("rm", "-fv", $exactId) `
            -TimeoutSeconds 10 -AllowFailure | Out-Null
    }
}

function Start-ZlinkSampleRedis {
    param(
        [Parameter(Mandatory = $true)][string]$Scope,
        [string]$Image = "redis:7.2-alpine",
        [Parameter(Mandatory = $true)]
        [ValidateSet("Java", "Kotlin")]
        [string]$Language
    )

    $pool = Get-ZlinkSamplePortPool -Language $Language
    $rangeSize = $pool.RedisMaximum - $pool.RedisMinimum + 1
    $startOffset = Get-Random -Minimum 0 -Maximum $rangeSize
    for ($offset = 0; $offset -lt $rangeSize; $offset++) {
        $hostPort = $pool.RedisMinimum + (($startOffset + $offset) % $rangeSize)
        if (-not (Test-ZlinkSampleTcpPortAvailable -Port $hostPort)) {
            continue
        }

        $name = "$Scope-$PID-$([Guid]::NewGuid().ToString('N'))-$hostPort"
        try {
            $created = Invoke-ZlinkDockerCommand -Arguments @(
                "create", "--name", $name, "--tmpfs", "/data", "-p",
                "127.0.0.1:${hostPort}:6379", $Image
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
            $started = Invoke-ZlinkDockerCommand -Arguments @(
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
            $running = (Invoke-ZlinkDockerCommand -Arguments @(
                "inspect", "-f", "{{.State.Running}}", $containerId
            )).Output
            $publishedPort = (Invoke-ZlinkDockerCommand -Arguments @(
                "inspect", "-f", '{{(index (index .NetworkSettings.Ports "6379/tcp") 0).HostPort}}', $containerId
            )).Output
            if ($running -ne "true" -or $publishedPort -ne "$hostPort") {
                throw "Failed to inspect the dedicated Redis container: $name"
            }
            Wait-ZlinkSampleRedisReady -ContainerId $containerId
            return [PSCustomObject]@{
                ContainerId = $containerId
                Endpoint = "127.0.0.1:$hostPort"
            }
        } catch {
            Remove-ZlinkSampleRedisAttempt -ContainerId $containerId -Name $name
            throw
        }
    }

    throw "No bindable $Language Redis host port remained in " +
        "$($pool.RedisMinimum)-$($pool.RedisMaximum) for $Scope."
}

function Remove-ZlinkSampleRedis {
    param([string]$ContainerId)
    if ($ContainerId -match '^[0-9a-f]{12,64}$') {
        Invoke-ZlinkDockerCommand -Arguments @("rm", "-fv", $ContainerId) -AllowFailure | Out-Null
    }
}

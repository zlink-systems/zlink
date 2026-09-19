Set-StrictMode -Version Latest

if (-not (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue)) {
    $IsWindows = $env:OS -eq "Windows_NT"
}

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
            $ports = if ($offset) { @($candidate, ($candidate + $offset)) } else { @($candidate) }
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

function Close-ZlinkSampleRunDir {
    <#
        Single owner of every C++ sample runner's run-directory lifetime.

        A passing run removes the directory, generated role config and all. A failing
        run keeps it and prints the path, because the role stdout/stderr logs inside
        it are the only evidence of why the run failed and a runner that deletes them
        on the failure path leaves nothing to diagnose.
    #>
    param(
        [Parameter(Mandatory = $true)][AllowEmptyString()][AllowNull()][string]$RunDir,
        [Parameter(Mandatory = $true)][int]$Status,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if ([string]::IsNullOrEmpty($RunDir) -or -not (Test-Path -LiteralPath $RunDir)) {
        return
    }
    if ($Status -ne 0) {
        Write-Host "$Label run directory preserved: $RunDir"
        return
    }
    Remove-Item -Recurse -Force -LiteralPath $RunDir -ErrorAction SilentlyContinue
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
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    if ($startInfo.PSObject.Properties.Name -contains 'ArgumentList') {
        foreach ($argument in $Arguments) {
            $startInfo.ArgumentList.Add($argument)
        }
    } else {
        $startInfo.Arguments = (($Arguments | ForEach-Object {
            '"' + $_.Replace('"', '\"') + '"'
        }) -join ' ')
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Failed to start Docker: docker $($Arguments -join ' ')"
    }
    try {
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            if ($env:OS -eq 'Windows_NT') {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            } else {
                $process.Kill($true)
            }
            throw "Docker command timed out after ${TimeoutSeconds}s: docker $($Arguments -join ' ')"
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult().Trim()
        $stderr = $stderrTask.GetAwaiter().GetResult().Trim()
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

function Get-ZlinkSampleSelfShellPath {
    <#
        Resolves the executable to relaunch the *current* PowerShell host as a child process
        (used by samples that isolate a scenario in its own process lane).

        Two things that do NOT work reliably and must not be reintroduced:
        - Hardcoding "powershell.exe": true only for Windows PowerShell 5.1 (Desktop edition).
          pwsh 7 (Core edition) ships "pwsh.exe"/"pwsh", so a literal name breaks one host or
          the other.
        - Introspecting the running process image via (Get-Process -Id $PID).Path: when pwsh is
          installed as a dotnet global tool, the OS-visible image for the running Core-edition
          process is dotnet.exe hosting the managed pwsh.dll, not a directly relaunchable
          pwsh.exe/pwsh shim. Passing that path back to Start-Process reaches dotnet.exe with the
          intended shell arguments folded into one unusable blob.

        Instead this resolves the name from $PSVersionTable.PSEdition (Desktop -> powershell.exe,
        Core -> pwsh[.exe]) and looks it up under $PSHOME, which names the PowerShell
        installation directory rather than the resolved OS process image and holds the real,
        directly-relaunchable executable in both hosts (including the dotnet-tool install
        layout). A PATH lookup is the fallback for layouts where $PSHOME does not hold it.
    #>
    $exeName = if ($PSVersionTable.PSEdition -eq "Desktop") {
        "powershell.exe"
    } elseif ($IsWindows -or $env:OS -eq "Windows_NT") {
        "pwsh.exe"
    } else {
        "pwsh"
    }

    $underPsHome = Join-Path $PSHOME $exeName
    if (Test-Path -LiteralPath $underPsHome) { return $underPsHome }

    $onPath = Get-Command $exeName -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($onPath) { return $onPath.Source }

    throw "Could not locate the current PowerShell host executable ($exeName) to relaunch a child lane."
}

# The C++ tree this runner's binaries are built in: the framework checkout when
# samples/ sits inside one (framework/languages/cpp/build), otherwise the root
# of the downloaded samples archive (bootstrap.cmake configures its build/).
# One rule, mirrored by samples/sample-build-common.sh for the Bash runners.
function Get-ZlinkCppSampleTreeRoot {
    $samplesRoot = (Resolve-Path (Join-Path $PSScriptRoot ".")).Path
    $cppRoot = Join-Path $samplesRoot ".."
    if ((Test-Path (Join-Path $cppRoot "CMakeLists.txt") -PathType Leaf) -and
        (Test-Path (Join-Path $cppRoot "framework/include/zlink/framework.hpp") -PathType Leaf)) {
        return (Resolve-Path $cppRoot).Path
    }
    return $samplesRoot
}

# Framework gates a runner runs beside its sample. The named tests exist only
# in the repository tree's build; the package tree (the downloaded samples
# archive) has none, so the gate is reported as skipped rather than silently
# passed. Mirrors zlink_cpp_sample_run_framework_tests in sample-build-common.sh.
function Invoke-ZlinkSampleFrameworkTests {
    param(
        [Parameter(Mandatory = $true)][string]$BuildDir,
        [Parameter(Mandatory = $true)][string]$Configuration,
        [Parameter(Mandatory = $true)][string]$Regex
    )
    $treeRoot = Get-ZlinkCppSampleTreeRoot
    if (-not (Test-Path (Join-Path $treeRoot "framework/include/zlink/framework.hpp") -PathType Leaf)) {
        Write-Host "framework tests: skipped (package tree; no framework test targets)"
        return
    }
    $ctest = if ($env:CTEST_BIN) { $env:CTEST_BIN } else { "ctest" }
    $previous = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        & $ctest --test-dir $BuildDir -C $Configuration -R $Regex --output-on-failure
        $status = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    if ($status -ne 0) {
        throw "framework tests failed with exit code $status"
    }
}

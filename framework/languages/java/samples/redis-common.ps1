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

function Set-ZlinkSampleJavaRuntime {
    param([Parameter(Mandatory = $true)][string]$SamplesRoot)

    $baselinePath = Join-Path $SamplesRoot "gradle/zlink-jvm-baseline.settings.gradle.kts"
    $baselineText = [System.IO.File]::ReadAllText($baselinePath)
    $versionMatch = [regex]::Match(
        $baselineText,
        '(?m)^val zlinkJavaLanguageVersion = ([0-9]+)[ \t]*\r?$')
    if (-not $versionMatch.Success) {
        throw "Java language version was not found in $baselinePath"
    }
    $requiredVersion = $versionMatch.Groups[1].Value

    # The same places gradle/zlink-jvm-runtime.sh looks in, in the same order:
    # JAVA_HOME, the java on PATH, the installations Gradle was configured with,
    # and the directories Gradle auto-detects.
    $candidates = [System.Collections.Generic.List[string]]::new()
    if ($env:JAVA_HOME) {
        $candidates.Add($env:JAVA_HOME)
    }
    $pathJava = Get-Command java -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($pathJava) {
        $candidates.Add((Split-Path -Parent (Split-Path -Parent $pathJava.Source)))
    }
    $userHome = if ($IsWindows) { $env:USERPROFILE } else { $env:HOME }
    $gradleHome = Join-Path $userHome ".gradle"
    $gradleProperties = Join-Path $gradleHome "gradle.properties"
    if (Test-Path -LiteralPath $gradleProperties -PathType Leaf) {
        $installationLine = Select-String -LiteralPath $gradleProperties `
            -Pattern '^org\.gradle\.java\.installations\.paths=(.+)$' |
            Select-Object -First 1
        if ($installationLine) {
            foreach ($candidate in $installationLine.Matches[0].Groups[1].Value.Split(',')) {
                $candidates.Add($candidate.Trim().Replace('\\', '\'))
            }
        }
    }
    $roots = [System.Collections.Generic.List[string]]::new()
    $roots.Add((Join-Path $gradleHome "jdks"))
    $roots.Add((Join-Path $userHome ".sdkman/candidates/java"))
    $roots.Add("/usr/lib/jvm")
    $roots.Add("/usr/java")
    $roots.Add("/Library/Java/JavaVirtualMachines")
    if ($env:LOCALAPPDATA) {
        $roots.Add((Join-Path $env:LOCALAPPDATA "Programs/jdk"))
    }
    if ($env:ProgramFiles) {
        foreach ($vendor in @("Java", "Eclipse Adoptium", "Microsoft")) {
            $roots.Add((Join-Path $env:ProgramFiles $vendor))
        }
    }
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) {
            continue
        }
        foreach ($entry in Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue) {
            $candidates.Add($entry.FullName)
            # macOS bundles keep the JDK one level down.
            $bundleHome = Join-Path $entry.FullName "Contents/Home"
            if (Test-Path -LiteralPath $bundleHome -PathType Container) {
                $candidates.Add($bundleHome)
            }
        }
    }

    foreach ($candidate in $candidates) {
        $releasePath = Join-Path $candidate "release"
        if (-not (Test-Path -LiteralPath $releasePath -PathType Leaf)) {
            continue
        }
        $releaseText = [System.IO.File]::ReadAllText($releasePath)
        $releasePattern = '(?m)^JAVA_VERSION="' +
            [regex]::Escape($requiredVersion) + '(?:\.|\")'
        if ($releaseText -match $releasePattern) {
            $env:JAVA_HOME = [System.IO.Path]::GetFullPath($candidate)
            $javaBin = Join-Path $env:JAVA_HOME 'bin'
            if (($env:PATH -split [System.IO.Path]::PathSeparator) -notcontains $javaBin) {
                $env:PATH = "$javaBin$([System.IO.Path]::PathSeparator)$env:PATH"
            }
            return
        }
    }
    throw ("JDK $requiredVersion was not found on this machine. " +
        "Gradle compiles these projects with the Java $requiredVersion toolchain pinned in " +
        "$baselinePath, so the installDist launchers need a JDK $requiredVersion runtime. " +
        "Install JDK $requiredVersion, or set JAVA_HOME to an existing JDK $requiredVersion installation.")
}

# Every sample child process starts here. Windows PowerShell 5.1 hands back a
# Process object that never opened the OS handle, so the exit code is gone the
# moment the child exits and `$process.ExitCode` reads back as $null instead of
# a number. Reading Handle while the child is alive keeps the exit code
# readable afterwards, on both PowerShell editions.
function Start-ZlinkSampleProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [string]$RedirectStandardOutput,
        [string]$RedirectStandardError
    )

    $parameters = @{
        FilePath = $FilePath
        WorkingDirectory = $WorkingDirectory
        NoNewWindow = $true
        PassThru = $true
    }
    foreach ($name in @("ArgumentList", "RedirectStandardOutput", "RedirectStandardError")) {
        if ($PSBoundParameters.ContainsKey($name)) {
            $parameters[$name] = $PSBoundParameters[$name]
        }
    }
    $process = Start-Process @parameters
    [void]$process.Handle
    return $process
}

function Invoke-ZlinkSampleExecutable {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$OutputPath
    )

    $errorPath = "$OutputPath.err.log"
    $argumentLine = ($Arguments | ForEach-Object {
        ConvertTo-ZlinkSampleProcessArgument $_
    }) -join " "
    $process = Start-ZlinkSampleProcess -FilePath $Executable -ArgumentList $argumentLine `
        -WorkingDirectory (Get-Location).Path `
        -RedirectStandardOutput $OutputPath -RedirectStandardError $errorPath
    try {
        $process.WaitForExit()
        $exitCode = [int]$process.ExitCode
    } finally {
        Stop-ZlinkSampleProcessTree -Process $process
        $process.Dispose()
    }
    if ($exitCode -ne 0) {
        throw "Sample command failed (exit=$exitCode): $Executable $argumentLine"
    }
}

function Stop-ZlinkSampleProcessTree {
    param([Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process)

    if ($Process.HasExited) { return }
    if (-not $IsWindows) {
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        return
    }

    $processId = $Process.Id
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = "taskkill.exe"
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.Arguments = "/PID $processId /T /F"

    $taskkill = [System.Diagnostics.Process]::new()
    $taskkill.StartInfo = $startInfo
    try {
        if (-not $taskkill.Start()) { throw "Failed to start taskkill.exe." }
        $stdout = $taskkill.StandardOutput.ReadToEndAsync()
        $stderr = $taskkill.StandardError.ReadToEndAsync()
        if (-not $taskkill.WaitForExit(5000)) {
            $taskkill.Kill()
            throw "taskkill.exe timed out while terminating process $processId."
        }
        if ($taskkill.ExitCode -ne 0 -and
            $null -ne (Get-Process -Id $processId -ErrorAction SilentlyContinue)) {
            throw "taskkill.exe failed for process $processId`: $($stderr.GetAwaiter().GetResult().Trim()) $($stdout.GetAwaiter().GetResult().Trim())"
        }
    } finally {
        $taskkill.Dispose()
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
        [string]$SettingsPath = "",
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
        $temporarySettingsPath = $null
        if ($SettingsPath) {
            $settingsSourcePath = Join-Path (Get-Location) $SettingsPath
            $settingsTargetPath = Join-Path (Get-Location) "settings.gradle.kts"
            if (-not (Test-Path -LiteralPath $settingsSourcePath -PathType Leaf)) {
                throw "Missing standalone Gradle settings: $settingsSourcePath"
            }
            if (Test-Path -LiteralPath $settingsTargetPath) {
                # A run killed hard leaves the staged copy behind. The staged copy
                # is ours only while it is a plain file byte-identical to the
                # standalone source; anything else is the developer's own settings
                # file and is never replaced.
                $existingSettings = Get-Item -LiteralPath $settingsTargetPath -Force
                $stagedCopy = ($existingSettings -is [System.IO.FileInfo]) -and
                    -not ($existingSettings.Attributes.HasFlag(
                        [System.IO.FileAttributes]::ReparsePoint)) -and
                    (Get-FileHash -LiteralPath $settingsSourcePath -Algorithm SHA256).Hash -eq
                        (Get-FileHash -LiteralPath $settingsTargetPath -Algorithm SHA256).Hash
                if (-not $stagedCopy) {
                    throw "Refusing to replace existing $settingsTargetPath"
                }
                [Console]::Error.WriteLine(
                    "Taking over the $settingsTargetPath left by an interrupted run.")
                Remove-Item -LiteralPath $settingsTargetPath -Force
            }
            Copy-Item -LiteralPath $settingsSourcePath -Destination $settingsTargetPath
            $temporarySettingsPath = $settingsTargetPath
        }
        $previousErrorActionPreference = $ErrorActionPreference
        try {
            # Windows PowerShell wraps legitimate Gradle stderr warnings as
            # NativeCommandError records. The native exit code remains the verdict.
            $ErrorActionPreference = "Continue"
            & $GradleExecutable @Arguments
            $gradleExitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousErrorActionPreference
        }
        if ($gradleExitCode -ne 0) {
            throw "Gradle build failed: $($Arguments -join ' ')"
        }
        if ($Arguments -match ':installDist$') {
            Optimize-ZlinkSampleWindowsLaunchers -Root (Get-Location).Path
        }
    } finally {
        if ($temporarySettingsPath) {
            Remove-Item -LiteralPath $temporarySettingsPath -Force -ErrorAction SilentlyContinue
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
        # Drain both pipes before waiting. A synchronous ReadToEnd after
        # WaitForExit deadlocks once the child fills a pipe buffer.
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

function Assert-ZlinkSampleSourcePolicy {
    param(
        [Parameter(Mandatory = $true)][string[]]$Path,
        [Parameter(Mandatory = $true)][string[]]$Extension,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$Message
    )

    # Matches the shell runners' rg scan: case-sensitive, and every source file is
    # read through an extended-length path so the scan never silently skips one.
    $regex = [regex]::new($Pattern)
    $offenders = @()
    foreach ($file in Get-ChildItem -LiteralPath $Path -Recurse -File) {
        if ($Extension -notcontains $file.Extension) { continue }
        $lineNumber = 0
        $extendedPath = '\\?\' + [IO.Path]::GetFullPath($file.FullName)
        foreach ($line in [IO.File]::ReadLines($extendedPath)) {
            $lineNumber++
            if ($regex.IsMatch($line)) { $offenders += "$($file.FullName):${lineNumber}:$line" }
        }
    }
    if ($offenders.Count -gt 0) {
        $offenders | ForEach-Object { [Console]::Error.WriteLine($_) }
        throw $Message
    }
}

function Get-ZlinkSamplePythonCommand {
    <#
        Resolves a working Python 3 interpreter for ZoneWorld's ZW-B8 fault proxy
        (used by the Java and Kotlin runners, which dot-source this shared file).

        A bare PATH lookup is not enough on Windows, for three separate reasons:
        - The python.org installer leaves "Add python.exe to PATH" unchecked by default,
          so a perfectly good per-user install is invisible to Get-Command.
        - The `py` launcher is installed to a directory of its own, and needs "-3" to
          select an interpreter rather than reading a shebang.
        - Windows 11 ships App Execution Alias stubs named python.exe/python3.exe under
          %LOCALAPPDATA%\Microsoft\WindowsApps, on PATH by default. They resolve, they
          launch, and then they refuse to run anything (exit 9009) and send the user to
          the Store. Finding an executable therefore does not mean finding Python.

        So candidates are gathered in preference order -- PATH, then the `py` launcher,
        then the standard per-user and machine install roots, newest version first --
        and each one only counts once it has actually reported a Python 3 version.
    #>
    $candidates = [System.Collections.Generic.List[object]]::new()
    $addCandidate = {
        param([string]$Path, [string[]]$Arguments)
        if ($Path -and (Test-Path -LiteralPath $Path -PathType Leaf)) {
            $candidates.Add([pscustomobject]@{ Path = $Path; Arguments = $Arguments })
        }
    }

    foreach ($name in @("python3", "python")) {
        foreach ($command in @(Get-Command $name -CommandType Application -ErrorAction SilentlyContinue)) {
            & $addCandidate $command.Source @()
        }
    }
    foreach ($command in @(Get-Command "py" -CommandType Application -ErrorAction SilentlyContinue)) {
        & $addCandidate $command.Source @("-3")
    }

    if ($IsWindows) {
        foreach ($launcher in @(
            (Join-Path $env:LOCALAPPDATA "Programs\Python\Launcher\py.exe"),
            (Join-Path $env:WINDIR "py.exe"))) {
            & $addCandidate $launcher @("-3")
        }
        foreach ($root in @(
            (Join-Path $env:LOCALAPPDATA "Programs\Python"),
            $env:ProgramFiles,
            ${env:ProgramFiles(x86)})) {
            if ([string]::IsNullOrWhiteSpace($root)) { continue }
            Get-ChildItem -LiteralPath $root -Directory -Filter "Python3*" -ErrorAction SilentlyContinue |
                Sort-Object -Property @{ Expression = { [int]($_.Name -replace '\D', '') } } -Descending |
                ForEach-Object { & $addCandidate (Join-Path $_.FullName "python.exe") @() }
        }
    }

    foreach ($candidate in $candidates) {
        $version = $null
        try {
            $version = (& $candidate.Path @(@($candidate.Arguments) + @("--version")) 2>&1 | Out-String)
        } catch {
            continue
        }
        if ($LASTEXITCODE -eq 0 -and $version -match "Python 3\.") {
            return $candidate
        }
    }

    throw ("Python 3 is required for the ZW-B8 fault proxy, and none was found on PATH, " +
        "through the py launcher, or under the standard install directories. " +
        "Install it from python.org or add an existing install to PATH.")
}

function Get-ZlinkSampleSelfShellPath {
    <#
        Resolves the executable to relaunch the *current* PowerShell host as a child process
        (used by ZoneWorld's isolated crash/routing lanes in both the Java and Kotlin runners,
        which dot-source this shared file).

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
    } elseif ($IsWindows) {
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

# Every Java and Kotlin sample runner dot-sources this file, so pin the JDK
# here rather than in each of the fourteen. The per-language batch runner used
# to do it once for the whole set; with samples running one at a time (#405)
# a runner that skipped it built with the toolchain JDK and launched its roles
# with whatever java came first on PATH, and the roles died on
# UnsupportedClassVersionError before signalling readiness.
Set-ZlinkSampleJavaRuntime -SamplesRoot (Join-Path $PSScriptRoot '')

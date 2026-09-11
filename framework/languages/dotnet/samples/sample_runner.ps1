Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue)) {
    $IsWindows = $env:OS -eq "Windows_NT"
}

$script:SampleProcesses = @()
$script:SampleProcessNames = @{}

if ($IsWindows -and -not ("Zlink.SampleWindowsProcessGroup" -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace Zlink
{
    public static class SampleWindowsProcessGroup
    {
        private const uint CreateNewProcessGroup = 0x00000200;
        private const uint CtrlBreakEvent = 1;
        private const uint GenericWrite = 0x40000000;
        private const uint FileShareRead = 0x00000001;
        private const uint FileShareWrite = 0x00000002;
        private const uint FileShareDelete = 0x00000004;
        private const uint CreateAlways = 2;
        private const uint FileAttributeNormal = 0x00000080;
        private const uint StartfUseStdHandles = 0x00000100;
        private const int StdInputHandle = -10;
        private static readonly IntPtr InvalidHandleValue = new IntPtr(-1);

        [StructLayout(LayoutKind.Sequential)]
        private struct SecurityAttributes
        {
            public int Length;
            public IntPtr SecurityDescriptor;
            [MarshalAs(UnmanagedType.Bool)] public bool InheritHandle;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct StartupInfo
        {
            public int Size;
            public string Reserved;
            public string Desktop;
            public string Title;
            public int X;
            public int Y;
            public int XSize;
            public int YSize;
            public int XCountChars;
            public int YCountChars;
            public int FillAttribute;
            public int Flags;
            public short ShowWindow;
            public short Reserved2Size;
            public IntPtr Reserved2;
            public IntPtr StandardInput;
            public IntPtr StandardOutput;
            public IntPtr StandardError;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct ProcessInformation
        {
            public IntPtr Process;
            public IntPtr Thread;
            public int ProcessId;
            public int ThreadId;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CreateProcess(
            string applicationName,
            StringBuilder commandLine,
            IntPtr processAttributes,
            IntPtr threadAttributes,
            [MarshalAs(UnmanagedType.Bool)] bool inheritHandles,
            uint creationFlags,
            IntPtr environment,
            string currentDirectory,
            ref StartupInfo startupInfo,
            out ProcessInformation processInformation);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CreateFile(
            string fileName,
            uint desiredAccess,
            uint shareMode,
            ref SecurityAttributes securityAttributes,
            uint creationDisposition,
            uint flagsAndAttributes,
            IntPtr templateFile);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CloseHandle(IntPtr handle);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr GetStdHandle(int standardHandle);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GenerateConsoleCtrlEvent(uint controlEvent, uint processGroupId);

        public static Process Start(
            string filePath,
            string[] arguments,
            string workingDirectory,
            string standardOutputPath,
            string standardErrorPath)
        {
            SecurityAttributes security = new SecurityAttributes
            {
                Length = Marshal.SizeOf(typeof(SecurityAttributes)),
                InheritHandle = true
            };
            IntPtr standardOutput = OpenLog(standardOutputPath, ref security);
            IntPtr standardError = IntPtr.Zero;
            ProcessInformation processInformation = new ProcessInformation();
            try
            {
                standardError = OpenLog(standardErrorPath, ref security);
                StartupInfo startupInfo = new StartupInfo
                {
                    Size = Marshal.SizeOf(typeof(StartupInfo)),
                    Flags = (int)StartfUseStdHandles,
                    StandardInput = GetStdHandle(StdInputHandle),
                    StandardOutput = standardOutput,
                    StandardError = standardError
                };
                string applicationName = filePath;
                StringBuilder commandLine;
                string extension = System.IO.Path.GetExtension(filePath);
                if (String.Equals(extension, ".cmd", StringComparison.OrdinalIgnoreCase) ||
                    String.Equals(extension, ".bat", StringComparison.OrdinalIgnoreCase))
                {
                    string commandInterpreter = Environment.GetEnvironmentVariable("ComSpec");
                    if (String.IsNullOrWhiteSpace(commandInterpreter))
                        throw new InvalidOperationException("ComSpec is required to launch a Windows batch command.");
                    applicationName = commandInterpreter;
                    commandLine = new StringBuilder(Quote(commandInterpreter)).Append(" /d /s /c \"")
                        .Append(BuildCommandLine(filePath, arguments)).Append('"');
                }
                else
                {
                    commandLine = BuildCommandLine(filePath, arguments);
                }

                if (!CreateProcess(
                    applicationName,
                    commandLine,
                    IntPtr.Zero,
                    IntPtr.Zero,
                    true,
                    CreateNewProcessGroup,
                    IntPtr.Zero,
                    String.IsNullOrWhiteSpace(workingDirectory) ? null : workingDirectory,
                    ref startupInfo,
                    out processInformation))
                {
                    throw new Win32Exception(Marshal.GetLastWin32Error(),
                        "Failed to start the sample process group.");
                }

                Process process = Process.GetProcessById(processInformation.ProcessId);
                process.Refresh();
                return process;
            }
            finally
            {
                if (processInformation.Thread != IntPtr.Zero) CloseHandle(processInformation.Thread);
                if (processInformation.Process != IntPtr.Zero) CloseHandle(processInformation.Process);
                if (standardError != IntPtr.Zero && standardError != InvalidHandleValue) CloseHandle(standardError);
                if (standardOutput != IntPtr.Zero && standardOutput != InvalidHandleValue) CloseHandle(standardOutput);
            }
        }

        public static void SendBreak(int processGroupId)
        {
            if (!GenerateConsoleCtrlEvent(CtrlBreakEvent, unchecked((uint)processGroupId)))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "Failed to send CTRL_BREAK_EVENT to sample process group " + processGroupId + ".");
            }
        }

        private static IntPtr OpenLog(string path, ref SecurityAttributes security)
        {
            IntPtr handle = CreateFile(
                path,
                GenericWrite,
                FileShareRead | FileShareWrite | FileShareDelete,
                ref security,
                CreateAlways,
                FileAttributeNormal,
                IntPtr.Zero);
            if (handle == InvalidHandleValue)
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Failed to open sample log " + path + ".");
            }
            return handle;
        }

        private static StringBuilder BuildCommandLine(string filePath, string[] arguments)
        {
            StringBuilder commandLine = new StringBuilder(Quote(filePath));
            foreach (string argument in arguments)
            {
                commandLine.Append(' ').Append(Quote(argument));
            }
            return commandLine;
        }

        private static string Quote(string argument)
        {
            if (argument.Length > 0 && argument.IndexOfAny(new[] { ' ', '\t', '\n', '\v', '"' }) < 0)
                return argument;

            StringBuilder quoted = new StringBuilder("\"");
            int backslashes = 0;
            foreach (char character in argument)
            {
                if (character == '\\')
                {
                    backslashes++;
                    continue;
                }
                if (character == '"')
                {
                    quoted.Append('\\', backslashes * 2 + 1).Append(character);
                    backslashes = 0;
                    continue;
                }
                quoted.Append('\\', backslashes).Append(character);
                backslashes = 0;
            }
            return quoted.Append('\\', backslashes * 2).Append('"').ToString();
        }
    }
}
'@
}

function Invoke-SampleDockerCommand {
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
    if ($startInfo.PSObject.Properties.Name -contains "ArgumentList") {
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
    try {
        if (-not $process.Start()) {
            throw "Failed to start docker command."
        }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            if ($IsWindows) {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            } else {
                $process.Kill($true)
            }
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

    if ($ContainerId -notmatch '^[0-9a-f]{12,64}$') { return }
    Invoke-SampleDockerCommand -Arguments @("rm", "-fv", $ContainerId) -AllowFailure | Out-Null
}

function Test-SampleTcpPortAvailable {
    param([Parameter(Mandatory = $true)][int]$Port)

    $listener = [System.Net.Sockets.TcpListener]::new(
        [System.Net.IPAddress]::Loopback,
        $Port)
    $listener.Server.ExclusiveAddressUse = $true
    try {
        $listener.Start()
        return $true
    }
    catch [System.Net.Sockets.SocketException] {
        return $false
    }
    finally {
        $listener.Stop()
    }
}

function Test-SampleDockerBindConflict {
    param([Parameter(Mandatory = $true)]$Result)

    $details = "$($Result.StdOut)`n$($Result.StdErr)"
    return $details -match '(?i)(address already in use|port is already allocated|failed to bind host port|bind for .* failed)'
}

function Remove-SampleRedisAttempt {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [string]$ContainerId = ""
    )

    if ($ContainerId -notmatch '^[0-9a-f]{12,64}$') {
        $inspected = Invoke-SampleDockerCommand -Arguments @(
            "inspect", "--type", "container", "-f", "{{.Id}}", $Name) -AllowFailure
        if ($inspected.ExitCode -eq 0 -and $inspected.StdOut -match '^[0-9a-f]{12,64}$') {
            $ContainerId = $inspected.StdOut
        }
    }
    if ($ContainerId -match '^[0-9a-f]{12,64}$') {
        Remove-SampleRedisContainer $ContainerId
    }
}

function Start-SampleRedisContainer {
    param(
        [Parameter(Mandatory = $true)][string]$Scope,
        [string]$Image = "redis:7.2-alpine"
    )

    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
        throw "Docker is required to run this sample."
    }

    $redisMinimumPort = 22000
    $redisMaximumPort = 22099
    $redisPoolSize = $redisMaximumPort - $redisMinimumPort + 1
    $startPort = Get-Random -Minimum $redisMinimumPort -Maximum ($redisMaximumPort + 1)

    for ($offset = 0; $offset -lt $redisPoolSize; $offset++) {
        $port = $redisMinimumPort + (($startPort - $redisMinimumPort + $offset) % $redisPoolSize)
        if (-not (Test-SampleTcpPortAvailable -Port $port)) {
            continue
        }

        $name = "$Scope-$PID-$([Guid]::NewGuid().ToString('N'))-$port"
        $containerId = ""
        try {
            $created = Invoke-SampleDockerCommand -Arguments @(
                "create", "--name", $name, "--tmpfs", "/data",
                "-p", "127.0.0.1:$($port):6379", $Image) -AllowFailure
        }
        catch {
            Remove-SampleRedisAttempt -Name $name -ContainerId $containerId
            throw
        }
        $containerId = if ($created.StdOut -match '^[0-9a-f]{12,64}$') {
            $created.StdOut
        }
        else {
            ""
        }

        if ($created.ExitCode -ne 0 -or -not $containerId) {
            Remove-SampleRedisAttempt -Name $name -ContainerId $containerId
            if (Test-SampleDockerBindConflict -Result $created) {
                continue
            }
            throw "Docker create failed for Redis container $name.`n$($created.StdErr)"
        }

        try {
            $started = Invoke-SampleDockerCommand -Arguments @(
                "start", $containerId) -AllowFailure
        }
        catch {
            Remove-SampleRedisAttempt -Name $name -ContainerId $containerId
            throw
        }
        if ($started.ExitCode -ne 0) {
            Remove-SampleRedisAttempt -Name $name -ContainerId $containerId
            if (Test-SampleDockerBindConflict -Result $started) {
                continue
            }
            throw "Docker start failed for Redis container $name.`n$($started.StdErr)"
        }

        try {
            $running = Invoke-SampleDockerCommand -Arguments @(
                "inspect", "-f", "{{.State.Running}}", $containerId)
            if ($running.StdOut -ne "true") {
                throw "Redis container $name did not enter the running state."
            }
            $publishedPort = Invoke-SampleDockerCommand -Arguments @(
                "inspect", "-f", "{{(index (index .NetworkSettings.Ports `"6379/tcp`") 0).HostPort}}", $containerId)
            if ($publishedPort.StdOut -ne "$port") {
                throw "Redis container $name did not publish the selected host port $port."
            }
            return [pscustomobject]@{
                ContainerId = $containerId
                Endpoint = "127.0.0.1:$port"
            }
        }
        catch {
            Remove-SampleRedisAttempt -Name $name -ContainerId $containerId
            throw
        }
    }

    throw "No Redis port is available within $redisMinimumPort-$redisMaximumPort."
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

    $applicationMinimumPort = 22100
    $applicationMaximumPort = 23999
    $poolSize = $applicationMaximumPort - $applicationMinimumPort + 1
    if ($Count -le 0 -or $Count -gt $poolSize) {
        throw "Sample port count must be between 1 and $poolSize."
    }

    if ($BasePort -gt 0) {
        $firstPort = $BasePort + 1
        $lastPort = $BasePort + $Count
        if ($firstPort -lt $applicationMinimumPort -or $lastPort -gt $applicationMaximumPort) {
            throw "Configured sample ports must stay within $applicationMinimumPort-$applicationMaximumPort."
        }
        $candidates = @($firstPort..$lastPort)
    }
    else {
        $startPort = Get-Random -Minimum $applicationMinimumPort -Maximum ($applicationMaximumPort + 1)
        $candidates = for ($offset = 0; $offset -lt $poolSize; $offset++) {
            $applicationMinimumPort + (($startPort - $applicationMinimumPort + $offset) % $poolSize)
        }
    }

    $ports = @()
    $listeners = @()
    try {
        foreach ($port in $candidates) {
            $listener = [System.Net.Sockets.TcpListener]::new(
                [System.Net.IPAddress]::Loopback,
                $port)
            $listener.Server.ExclusiveAddressUse = $true
            try {
                $listener.Start()
            }
            catch [System.Net.Sockets.SocketException] {
                $listener.Stop()
                if ($BasePort -gt 0) {
                    throw "Configured sample port $port is unavailable."
                }
                continue
            }
            $listeners += $listener
            $ports += $port
            if ($ports.Count -eq $Count) {
                break
            }
        }
        if ($ports.Count -ne $Count) {
            throw "Could not reserve $Count application ports within $applicationMinimumPort-$applicationMaximumPort."
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

function Start-SampleProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string]$LogDirectory,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = ""
    )

    $standardOutput = Join-Path $LogDirectory "$Name.out.log"
    $standardError = Join-Path $LogDirectory "$Name.err.log"
    if ($IsWindows) {
        $command = Get-Command $FilePath -CommandType Application -ErrorAction Stop |
            Select-Object -First 1
        $process = [Zlink.SampleWindowsProcessGroup]::Start(
            $command.Source,
            $Arguments,
            $WorkingDirectory,
            $standardOutput,
            $standardError)
    }
    else {
        $parameters = @{
            FilePath = $FilePath
            ArgumentList = $Arguments
            RedirectStandardOutput = $standardOutput
            RedirectStandardError = $standardError
            NoNewWindow = $true
            PassThru = $true
        }
        if ($WorkingDirectory) {
            $parameters.WorkingDirectory = $WorkingDirectory
        }
        $process = Start-Process @parameters
    }
    # Materialize the native process handle before callers inspect ExitCode.
    [void]$process.Handle
    $script:SampleProcesses += $process
    $script:SampleProcessNames[$process.Id] = $Name
    return $process
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
    return Start-SampleProcess -Name $Name -FilePath "dotnet" -Arguments $argumentList `
        -LogDirectory $LogDirectory
}

function Stop-SampleWindowsProcessTree {
    param([Parameter(Mandatory = $true)][int]$ProcessId)

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = "taskkill.exe"
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    if ($startInfo.PSObject.Properties.Name -contains "ArgumentList") {
        foreach ($argument in @("/PID", "$ProcessId", "/T", "/F")) {
            $startInfo.ArgumentList.Add($argument)
        }
    }
    else {
        $startInfo.Arguments = "/PID $ProcessId /T /F"
    }
    $taskkill = [System.Diagnostics.Process]::new()
    $taskkill.StartInfo = $startInfo
    try {
        if (-not $taskkill.Start()) { throw "Failed to start taskkill.exe." }
        $stdout = $taskkill.StandardOutput.ReadToEndAsync()
        $stderr = $taskkill.StandardError.ReadToEndAsync()
        if (-not $taskkill.WaitForExit(5000)) {
            $taskkill.Kill()
            throw "taskkill.exe timed out while terminating process $ProcessId."
        }
        if ($taskkill.ExitCode -ne 0 -and
            $null -ne (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)) {
            throw "taskkill.exe failed for process $ProcessId`: $($stderr.GetAwaiter().GetResult().Trim()) $($stdout.GetAwaiter().GetResult().Trim())"
        }
    }
    finally {
        $taskkill.Dispose()
    }
}

function Stop-SampleProcess {
    param(
        [Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process,
        [switch]$Force,
        [int]$GraceMilliseconds = 30000
    )

    if ($Process.HasExited) { return }
    if (-not $Force) {
        if ($IsWindows) {
            try {
                [Zlink.SampleWindowsProcessGroup]::SendBreak($Process.Id)
            }
            catch {
                $Process.Refresh()
                if (-not $Process.HasExited) { throw }
            }
        }
        else {
            try { [void]$Process.CloseMainWindow() } catch {}
        }
        if ($Process.WaitForExit($GraceMilliseconds)) { return }
    }
    if ($IsWindows) {
        Stop-SampleWindowsProcessTree -ProcessId $Process.Id
    }
    else {
        $Process.Kill($true)
    }
    if (-not $Process.WaitForExit(5000)) {
        throw "Sample process $($Process.Id) did not exit after termination."
    }
}

function Wait-SampleProcess {
    param(
        [Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)][string]$Description,
        [int]$TimeoutSeconds = 180
    )

    if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
        Stop-SampleProcess -Process $Process -Force
        throw "$Description timed out after $TimeoutSeconds seconds."
    }
    $Process.Refresh()
    if ($Process.ExitCode -ne 0) {
        throw "$Description failed with exit code $($Process.ExitCode)."
    }
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
    $teardownFailures = @()
    $forcedProcessIds = @{}
    for ($i = $script:SampleProcesses.Count - 1; $i -ge 0; $i--) {
        $process = $script:SampleProcesses[$i]
        try {
            if (-not $process.HasExited) {
                if ($IsWindows) {
                    [Zlink.SampleWindowsProcessGroup]::SendBreak($process.Id)
                }
                else {
                    $process.CloseMainWindow() | Out-Null
                }
                Start-Sleep -Milliseconds 100
            }
        }
        catch {
            $process.Refresh()
            if (-not $process.HasExited) {
                $name = $script:SampleProcessNames[$process.Id]
                if ([string]::IsNullOrWhiteSpace($name)) { $name = "pid-$($process.Id)" }
                $teardownFailures += "Sample role $name (pid $($process.Id)) graceful termination failed: $($_.Exception.Message)"
            }
        }
    }

    # Match the Framework shutdown deadline used by redis-common.sh.
    for ($i = 0; $i -lt 300; $i++) {
        $alive = @($script:SampleProcesses | Where-Object { -not $_.HasExited })
        if ($alive.Count -eq 0) {
            break
        }
        Start-Sleep -Milliseconds 100
    }

    foreach ($process in $script:SampleProcesses) {
        try {
            if (-not $process.HasExited) {
                $name = $script:SampleProcessNames[$process.Id]
                if ([string]::IsNullOrWhiteSpace($name)) { $name = "pid-$($process.Id)" }
                if ($IsWindows) {
                    $teardownFailures += "Sample role $name (pid $($process.Id)) required forced termination (taskkill /F)."
                }
                else {
                    $teardownFailures += "Sample role $name (pid $($process.Id)) exited during cleanup with status -9 (SIGKILL)."
                }
                $forcedProcessIds[$process.Id] = $true
                if ($IsWindows) {
                    Stop-SampleWindowsProcessTree -ProcessId $process.Id
                } else {
                    $process.Kill($true)
                }
            }
            if (-not $process.WaitForExit(5000)) {
                $name = $script:SampleProcessNames[$process.Id]
                if ([string]::IsNullOrWhiteSpace($name)) { $name = "pid-$($process.Id)" }
                $teardownFailures += "Sample role $name (pid $($process.Id)) did not exit after termination."
                continue
            }
            if (-not $forcedProcessIds.ContainsKey($process.Id) -and
                ($process.ExitCode -eq 137 -or $process.ExitCode -eq -9)) {
                $name = $script:SampleProcessNames[$process.Id]
                if ([string]::IsNullOrWhiteSpace($name)) { $name = "pid-$($process.Id)" }
                $message = "Sample role $name (pid $($process.Id)) exited during cleanup with status $($process.ExitCode)."
                if ($message -notin $teardownFailures) { $teardownFailures += $message }
            }
        }
        catch {
            $name = $script:SampleProcessNames[$process.Id]
            if ([string]::IsNullOrWhiteSpace($name)) { $name = "pid-$($process.Id)" }
            $teardownFailures += "Sample role $name (pid $($process.Id)) cleanup failed: $($_.Exception.Message)"
        }
        finally {
            $process.Dispose()
        }
    }
    if ($teardownFailures.Count -gt 0) {
        throw ($teardownFailures -join [Environment]::NewLine)
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

Set-StrictMode -Version Latest

function Resolve-ZlinkZoneWorldPython {
    $pythonCommand = Get-Command python.exe, python3.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1
    $pythonExecutable = if ($pythonCommand) {
        $pythonCommand.Source
    } else {
        $pythonRoot = Join-Path $env:LOCALAPPDATA "Programs\Python"
        Get-ChildItem -LiteralPath $pythonRoot -Filter python.exe -Recurse -File `
            -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
    }
    if ([string]::IsNullOrWhiteSpace($pythonExecutable) -or
            -not (Test-Path -LiteralPath $pythonExecutable -PathType Leaf)) {
        throw "Python 3 is required for the ZW-B8 proxy lane."
    }
    return (Resolve-Path -LiteralPath $pythonExecutable).Path
}

function Invoke-ZlinkZoneWorldSample {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet("Java", "Kotlin")]
        [string]$Language,
        [Parameter(Mandatory = $true)][string]$SampleDir,
        [Parameter(Mandatory = $true)][string]$RunnerPath,
        [string]$Scenario = "all",
        [switch]$G4Child,
        [switch]$B8Child
    )

    $ErrorActionPreference = "Stop"
    $languageKey = $Language.ToLowerInvariant()
    $Scenario = if ($Scenario -eq "full") { "all" } else { $Scenario }
    $selectedScenarios = @($Scenario.Split(",", [StringSplitOptions]::RemoveEmptyEntries))
    $isSelected = {
        param([string]$Id)
        return $Scenario -eq "all" -or $selectedScenarios -contains $Id
    }

    $powerShell = Get-Command pwsh.exe, powershell.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty Source
    if (-not $powerShell) {
        throw "PowerShell executable was not found."
    }

    $g4Proven = $false
    $b8Proven = $false
    if (-not $G4Child -and (& $isSelected "ZW-G4")) {
        & $powerShell -NoProfile -ExecutionPolicy Bypass -File $RunnerPath `
            -Scenario ZW-G4 -G4Child
        $g4Proven = $LASTEXITCODE -eq 0
        if (-not $g4Proven) {
            [Console]::Error.WriteLine("scenario ZW-G4 blocked: isolated crash lane failed")
        }
        if ($Scenario -eq "ZW-G4") {
            if ($g4Proven) { return }
            throw "scenario ZW-G4 failed"
        }
    }
    if (-not $B8Child -and (& $isSelected "ZW-B8")) {
        & $powerShell -NoProfile -ExecutionPolicy Bypass -File $RunnerPath `
            -Scenario ZW-B8 -B8Child
        $b8Proven = $LASTEXITCODE -eq 0
        if (-not $b8Proven) {
            [Console]::Error.WriteLine("scenario ZW-B8 blocked: isolated command-44 lane failed")
        }
        if ($Scenario -eq "ZW-B8") {
            if ($b8Proven) { return }
            throw "scenario ZW-B8 failed"
        }
    }

    $runDir = Join-Path ([IO.Path]::GetTempPath()) `
        "zlink-zoneworld-$languageKey-$PID-$([Guid]::NewGuid().ToString('N'))"
    $logDir = Join-Path $runDir "logs"
    $configDir = Join-Path $runDir "config"
    New-Item -ItemType Directory -Force -Path $logDir, $configDir | Out-Null

    $gradle = if ($IsWindows) {
        Join-Path $SampleDir "../../gradlew.bat"
    } else {
        Join-Path $SampleDir "../../gradlew"
    }
    $serverBin = Join-Path $SampleDir "Server/build/install/Server/bin/Server.bat"
    $clientBin = Join-Path $SampleDir "Client/build/install/Client/bin/Client.bat"
    $processes = [Collections.Generic.List[Diagnostics.Process]]::new()
    $nodeProcesses = @{}
    $ownedConsolePids = @{}
    $frameworkLogOffsets = @{}
    $redisContainerId = $null
    $state = [PSCustomObject]@{ Status = 0 }
    $runnerLog = Join-Path $logDir "runner.log"
    Set-ZlinkSampleUtf8File -Path $runnerLog -Value @()

    function Write-FailureLogs {
        Get-ChildItem -Path $logDir -Filter "*.log" -ErrorAction SilentlyContinue |
            ForEach-Object {
                [Console]::Error.WriteLine("===== $($_.FullName) =====")
                Get-Content -LiteralPath $_.FullName -Tail 100 -ErrorAction SilentlyContinue |
                    ForEach-Object { [Console]::Error.WriteLine($_) }
            }
    }

    $ownedConsoleSource = @'
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

public static class ZlinkWindowsOwnedConsole
{
    private const uint CREATE_NEW_CONSOLE = 0x00000010;
    private const uint CTRL_C_EVENT = 0;
    private const uint ATTACH_PARENT_PROCESS = 0xffffffff;
    private const uint STARTF_USESHOWWINDOW = 0x00000001;
    private const short SW_HIDE = 0;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct STARTUPINFO
    {
        public int cb;
        public string lpReserved;
        public string lpDesktop;
        public string lpTitle;
        public uint dwX;
        public uint dwY;
        public uint dwXSize;
        public uint dwYSize;
        public uint dwXCountChars;
        public uint dwYCountChars;
        public uint dwFillAttribute;
        public uint dwFlags;
        public short wShowWindow;
        public short cbReserved2;
        public IntPtr lpReserved2;
        public IntPtr hStdInput;
        public IntPtr hStdOutput;
        public IntPtr hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PROCESS_INFORMATION
    {
        public IntPtr hProcess;
        public IntPtr hThread;
        public uint dwProcessId;
        public uint dwThreadId;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateProcess(
        string applicationName,
        StringBuilder commandLine,
        IntPtr processAttributes,
        IntPtr threadAttributes,
        bool inheritHandles,
        uint creationFlags,
        IntPtr environment,
        string currentDirectory,
        ref STARTUPINFO startupInfo,
        out PROCESS_INFORMATION processInformation);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GenerateConsoleCtrlEvent(uint ctrlEvent, uint processGroupId);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool AttachConsole(uint processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool FreeConsole();

    [DllImport("kernel32.dll")]
    private static extern IntPtr GetConsoleWindow();

    private delegate bool HandlerRoutine(uint ctrlType);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetConsoleCtrlHandler(HandlerRoutine handler, bool add);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);

    public static Process Start(string commandLine, string currentDirectory)
    {
        STARTUPINFO startupInfo = new STARTUPINFO();
        startupInfo.cb = Marshal.SizeOf(typeof(STARTUPINFO));
        startupInfo.dwFlags = STARTF_USESHOWWINDOW;
        startupInfo.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION processInformation;
        if (!CreateProcess(null, new StringBuilder(commandLine), IntPtr.Zero, IntPtr.Zero,
                false, CREATE_NEW_CONSOLE, IntPtr.Zero, currentDirectory,
                ref startupInfo, out processInformation))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(),
                "Could not create the owned Windows console process.");
        }
        try
        {
            Process process = Process.GetProcessById(checked((int)processInformation.dwProcessId));
            IntPtr managedHandle = process.Handle;
            return process;
        }
        finally
        {
            CloseHandle(processInformation.hThread);
            CloseHandle(processInformation.hProcess);
        }
    }

    public static void SendInterrupt(int consoleOwnerProcessId)
    {
        bool hadConsole = GetConsoleWindow() != IntPtr.Zero;
        bool attachedToTarget = false;
        bool ignoringCtrlC = false;
        if (hadConsole)
        {
            FreeConsole();
        }
        try
        {
            if (!AttachConsole(checked((uint)consoleOwnerProcessId)))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "Could not attach to the owned Windows console.");
            }
            attachedToTarget = true;
            if (!SetConsoleCtrlHandler(null, true))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "Could not protect the sample runner from CTRL_C_EVENT.");
            }
            ignoringCtrlC = true;
            if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "Could not send CTRL_C_EVENT to the owned Windows console.");
            }
            System.Threading.Thread.Sleep(200);
        }
        finally
        {
            if (ignoringCtrlC)
            {
                SetConsoleCtrlHandler(null, false);
            }
            if (attachedToTarget)
            {
                FreeConsole();
            }
            if (hadConsole && !AttachConsole(ATTACH_PARENT_PROCESS))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(),
                    "Could not restore the sample runner console.");
            }
        }
    }
}
'@
    if (-not ([Management.Automation.PSTypeName]'ZlinkWindowsOwnedConsole').Type) {
        Add-Type -TypeDefinition $ownedConsoleSource
    }

    function Send-OwnedConsoleInterrupt {
        param([Parameter(Mandatory = $true)][int]$ConsoleOwnerProcessId)

        $quotedSource = "'" + $ownedConsoleSource.Replace("'", "''") + "'"
        $command = '$ErrorActionPreference = "Stop"; ' +
            "Add-Type -TypeDefinition $quotedSource; " +
            "[ZlinkWindowsOwnedConsole]::SendInterrupt($ConsoleOwnerProcessId)"
        $encodedCommand = [Convert]::ToBase64String(
            [Text.Encoding]::Unicode.GetBytes($command))
        $sender = Start-Process -FilePath $powerShell -ArgumentList @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-EncodedCommand", $encodedCommand) `
            -WindowStyle Hidden -Wait -PassThru
        if ($sender.ExitCode -ne 0) {
            throw "Could not deliver CTRL_C_EVENT to owned console PID $ConsoleOwnerProcessId."
        }
    }

    function Get-RunningProcessIds {
        param([Parameter(Mandatory = $true)][int[]]$ProcessIds)
        return @($ProcessIds | Sort-Object -Unique | Where-Object {
            $null -ne (Get-Process -Id $_ -ErrorAction SilentlyContinue)
        })
    }

    function Wait-ProcessIdsExit {
        param(
            [Parameter(Mandatory = $true)][int[]]$ProcessIds,
            [int]$TimeoutMilliseconds = 5000
        )
        $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
        do {
            $running = @(Get-RunningProcessIds -ProcessIds $ProcessIds)
            if ($running.Count -eq 0) { return }
            Start-Sleep -Milliseconds 50
        } while ([DateTime]::UtcNow -lt $deadline)
        throw "Process group leaked PID(s): $($running -join ', ')"
    }

    function Stop-AllProcesses {
        $failures = [Collections.Generic.List[string]]::new()
        for ($index = $processes.Count - 1; $index -ge 0; $index--) {
            try {
                Stop-ZlinkSampleProcessTree -Process $processes[$index] -Force
            } catch {
                $failures.Add($_.Exception.Message)
            }
        }
        foreach ($process in $processes) {
            if (-not $process.HasExited) {
                $failures.Add("Cleanup leaked wrapper PID $($process.Id).")
            }
        }
        if ($failures.Count -ne 0) {
            throw ($failures -join [Environment]::NewLine)
        }
    }

    function Protect-ConfigFile {
        param([Parameter(Mandatory = $true)][string]$Path)
        $identity = [Security.Principal.WindowsIdentity]::GetCurrent().Name
        & icacls.exe $Path /inheritance:r /grant:r "${identity}:(R,W)" *> $null
        if ($LASTEXITCODE -ne 0) {
            throw "Could not restrict config file ACL: $Path"
        }
    }

    function Write-ServerConfig {
        param(
            [string]$Name,
            [string]$Role,
            [string]$Node,
            [int]$MeshPort,
            [int]$StreamPort = 0,
            [bool]$SubscriberOnly = $false,
            [bool]$DisableBots = $false,
            [bool]$AllowEmptyZoneSet = $false,
            [string]$FaultTickZone = ""
        )
        $bindHost = "127.0.0.1"
        $advertiseHost = ""
        if ($B8Child -and $Role -ne "ops" -and -not $SubscriberOnly) {
            $bindHost = "127.0.0.2"
            $advertiseHost = "127.0.0.1"
        }
        $path = Join-Path $configDir "$Name.properties"
        Set-ZlinkSampleUtf8File -Path $path -Value @(
            "sample.role=$Role",
            "sample.node-id=$Node",
            "sample.mesh-endpoint=tcp://${bindHost}:$MeshPort",
            "sample.stream-endpoint=tcp://127.0.0.1:$StreamPort",
            "sample.redis-endpoint=$redisEndpoint",
            "sample.redis-key-prefix=$redisKeyPrefix",
            "sample.subscriber-only=$($SubscriberOnly.ToString().ToLowerInvariant())",
            "sample.disable-bots=$($DisableBots.ToString().ToLowerInvariant())",
            "sample.allow-empty-zone-set=$($AllowEmptyZoneSet.ToString().ToLowerInvariant())",
            "sample.fault-tick-zone=$FaultTickZone",
            "sample.mesh-advertise-host=$advertiseHost"
        )
        Protect-ConfigFile -Path $path
        return $path
    }

    function Start-LoggedProcess {
        param(
            [string]$Name,
            [string]$FilePath,
            [string[]]$Arguments = @(),
            [string[]]$ExpectedLeafProcessNames = @("java", "javaw"),
            [switch]$AllowExitedLauncher
        )
        $logPath = Join-Path $logDir "$Name.log"
        $quotedFilePath = "'" + $FilePath.Replace("'", "''") + "'"
        $quotedArguments = @($Arguments | ForEach-Object {
            "'" + ([string]$_).Replace("'", "''") + "'"
        }) -join " "
        $quotedLogPath = "'" + $logPath.Replace("'", "''") + "'"
        $command = '$ErrorActionPreference = "Stop"; ' +
            '$previousErrorActionPreference = $ErrorActionPreference; try { ' +
            '$ErrorActionPreference = "Continue"; ' +
            "& $quotedFilePath $quotedArguments *>> $quotedLogPath; " +
            '$exitCode = $LASTEXITCODE } finally { ' +
            '$ErrorActionPreference = $previousErrorActionPreference }; ' +
            'if ($null -ne $exitCode) { exit $exitCode }'
        $encodedCommand = [Convert]::ToBase64String(
            [Text.Encoding]::Unicode.GetBytes($command))
        $nativeArguments = @(
            $powerShell,
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-EncodedCommand", $encodedCommand
        ) | ForEach-Object { ConvertTo-ZlinkSampleProcessArgument ([string]$_) }
        $nativeCommandLine = $nativeArguments -join " "
        $previousTrace = $env:ZLINK_JAVA_STREAM_TRACE
        $env:ZLINK_JAVA_STREAM_TRACE = "1"
        try {
            $firstLogLine = Get-NextLine $logPath
            $process = [ZlinkWindowsOwnedConsole]::Start($nativeCommandLine, $SampleDir)
        } finally {
            $env:ZLINK_JAVA_STREAM_TRACE = $previousTrace
        }
        $processes.Add($process)
        Register-ZlinkSampleProcessTree -Process $process `
            -ExpectedLeafProcessNames $ExpectedLeafProcessNames `
            -AllowExitedLauncher:$AllowExitedLauncher
        $nodeProcesses[$Name] = $process
        $ownedConsolePids[$process.Id] = $process.Id
        if ([IO.Path]::GetFullPath($FilePath) -eq [IO.Path]::GetFullPath($serverBin)) {
            $frameworkLogOffsets[$Name] = $firstLogLine
        }
        Write-Host "    started $Name pid=$($process.Id)"
        return $process
    }

    function Get-NextLine {
        param([string]$Path)
        if (-not (Test-Path -LiteralPath $Path)) { return 1 }
        return @(Get-Content -LiteralPath $Path).Count + 1
    }

    function Test-LogContains {
        param([string]$Path, [string]$Pattern, [int]$FirstLine = 1, [switch]$Regex)
        if (-not (Test-Path -LiteralPath $Path)) { return $false }
        $lines = @(Get-Content -LiteralPath $Path | Select-Object -Skip ($FirstLine - 1))
        if ($Regex) { return $null -ne ($lines | Select-String -Pattern $Pattern) }
        return $null -ne ($lines | Select-String -Pattern $Pattern -SimpleMatch)
    }

    function Wait-Log {
        param(
            [string]$Name,
            [string]$Pattern,
            [int]$FirstLine = 1,
            [Diagnostics.Process]$Process = $null,
            [int]$Attempts = 600
        )
        $path = Join-Path $logDir "$Name.log"
        for ($attempt = 0; $attempt -lt $Attempts; $attempt++) {
            if (Test-LogContains -Path $path -Pattern $Pattern -FirstLine $FirstLine) { return }
            if ($Process -and $Process.HasExited) {
                throw "$Name exited before '$Pattern' was observed."
            }
            Start-Sleep -Milliseconds 100
        }
        throw "Timed out waiting for '$Pattern' in $Name after line $FirstLine"
    }

    function Get-RoutingId {
        param([string]$Name, [int]$FirstLine = 1)
        $path = Join-Path $logDir "$Name.log"
        if (-not (Test-Path -LiteralPath $path)) { return "" }
        $matches = Get-Content -LiteralPath $path | Select-Object -Skip ($FirstLine - 1) |
            Select-String -Pattern '\brid=(zn-[0-9a-f-]+)\b' -AllMatches
        if (-not $matches) { return "" }
        return $matches[-1].Matches[-1].Groups[1].Value
    }

    function Test-ZoneRoutingId {
        param([string]$Value)
        return $Value -match '^zn-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$'
    }

    function Stop-ZoneNode {
        param([string]$Name, [ValidateSet("TERM", "KILL")][string]$Mode = "KILL")
        if (-not $nodeProcesses.ContainsKey($Name)) { return }
        $process = $nodeProcesses[$Name]
        if (-not $process.HasExited) {
            if ($Mode -eq "KILL") {
                Stop-ZlinkSampleProcessTree -Process $process -Force
            } else {
                if (-not $frameworkLogOffsets.ContainsKey($Name)) {
                    throw "Framework lifecycle offset was not recorded for $Name."
                }
                $descendantIds = @(Get-ZlinkSampleDescendantProcessIds `
                    -ParentProcessId $process.Id)
                $javaIds = @($descendantIds | Where-Object {
                    $candidate = Get-Process -Id $_ -ErrorAction SilentlyContinue
                    $null -ne $candidate -and $candidate.ProcessName -in @("java", "javaw")
                })
                if ($javaIds.Count -eq 0) {
                    throw "No owned JVM descendant was found for $Name (PID $($process.Id))."
                }
                Send-OwnedConsoleInterrupt -ConsoleOwnerProcessId `
                    ([int]$ownedConsolePids[$process.Id])
                Wait-ProcessIdsExit -ProcessIds (@($process.Id) + $descendantIds)
                Assert-FrameworkGracefulTermination -Name $Name `
                    -FirstLine ([int]$frameworkLogOffsets[$Name])
            }
        }
        $nodeProcesses.Remove($Name)
        $ownedConsolePids.Remove($process.Id)
        $frameworkLogOffsets.Remove($Name)
        [void]$processes.Remove($process)
    }

    function Assert-FrameworkGracefulTermination {
        param([string]$Name, [int]$FirstLine)
        $logPath = Join-Path $logDir "$Name.log"
        if (-not (Test-Path -LiteralPath $logPath)) {
            throw "Framework role log is missing: $logPath"
        }
        $lines = @(Get-Content -LiteralPath $logPath | Select-Object -Skip ($FirstLine - 1))
        $readyCount = @($lines | Select-String -SimpleMatch "ZLINK_FRAMEWORK_READY").Count
        $terminationCount = @($lines | Select-String -SimpleMatch `
            "ZLINK_FRAMEWORK_TERMINATION outcome=").Count
        $stoppedCount = @($lines | Select-String -SimpleMatch `
            "ZLINK_FRAMEWORK_TERMINATION outcome=STOPPED reason=NONE").Count
        $forceStoppedCount = @($lines | Select-String -SimpleMatch `
            "ZLINK_FRAMEWORK_TERMINATION outcome=FORCE_STOPPED").Count
        if ($readyCount -ne 1 -or $terminationCount -ne 1 -or
                $stoppedCount -ne 1 -or $forceStoppedCount -ne 0) {
            throw "Framework lifecycle evidence is incomplete for $Name`: " +
                "READY=$readyCount TERMINATION=$terminationCount " +
                "STOPPED_NONE=$stoppedCount FORCE_STOPPED=$forceStoppedCount"
        }
    }

    function Start-Zone {
        param(
            [string]$Name,
            [string]$ConfigName = $Name,
            [bool]$WaitForStatusReport = $true
        )
        $firstLine = Get-NextLine (Join-Path $logDir "$Name.log")
        $process = Start-LoggedProcess -Name $Name -FilePath $serverBin `
            -Arguments @("--config", (Join-Path $configDir "$ConfigName.properties"))
        Wait-Log -Name $Name -Pattern "topology=ready" -FirstLine $firstLine `
            -Process $process -Attempts 900
        if ($WaitForStatusReport) {
            Wait-Log -Name $Name -Pattern "node status report submitted" -FirstLine $firstLine `
                -Process $process -Attempts 900
        }
    }

    function New-ClientConfig {
        param([string]$Id)
        $path = Join-Path $configDir "client-$($Id.Replace(',', '-')).properties"
        Set-ZlinkSampleUtf8File -Path $path -Value @(
            "sample.gateway-endpoint=tcp://127.0.0.1:$gatewayStream",
            "sample.ops-endpoint=tcp://127.0.0.1:$opsStream",
            "sample.scenarios=$Id",
            "sample.stream-trace=true",
            "sample.fault-arm-file=$(Join-Path $runDir 'b8-block-command-44')"
        )
        Protect-ConfigFile -Path $path
        return $path
    }

    function Start-Client {
        param([string]$Id)
        $name = "client-$($Id.Replace(',', '-'))-$([Guid]::NewGuid().ToString('N'))"
        return Start-LoggedProcess -Name $name -FilePath $clientBin `
            -Arguments @("--config", (New-ClientConfig -Id $Id)) -AllowExitedLauncher
    }

    function Complete-Client {
        param([Diagnostics.Process]$Process, [string]$Name)
        $Process.WaitForExit()
        $source = Join-Path $logDir "$Name.log"
        $errorSource = Join-Path $logDir "$Name.err.log"
        $clientLog = Join-Path $logDir "client.log"
        foreach ($path in @($source, $errorSource)) {
            if (Test-Path -LiteralPath $path) {
                Get-Content -LiteralPath $path |
                    Tee-Object -FilePath $clientLog -Append | ForEach-Object { Write-Host $_ }
            }
        }
        $nodeProcesses.Remove($Name)
        [void]$processes.Remove($Process)
        $ownedConsolePids.Remove($Process.Id)
        return $Process.ExitCode -eq 0
    }

    function Invoke-Client {
        param([string]$Id)
        $process = Start-Client -Id $Id
        $name = ($nodeProcesses.GetEnumerator() | Where-Object { $_.Value.Id -eq $process.Id }).Key
        return Complete-Client -Process $process -Name $name
    }

    function Add-Pass {
        param([string]$Id)
        "scenario $Id passed" | Tee-Object -FilePath $runnerLog -Append
    }

    function Add-Failure {
        param([string]$Id, [string]$Reason)
        "scenario $Id failed" | Tee-Object -FilePath $runnerLog -Append |
            ForEach-Object { [Console]::Error.WriteLine($_) }
        [Console]::Error.WriteLine("    $Reason")
        $state.Status = 1
    }

    function Test-AllLogs {
        param([string]$Id, [string]$Pattern, [string[]]$Names)
        if (-not (& $isSelected $Id)) { return }
        foreach ($name in $Names) {
            if (-not (Test-LogContains -Path (Join-Path $logDir $name) -Pattern $Pattern)) {
                Add-Failure $Id "missing '$Pattern' in $name"
                return
            }
        }
        Add-Pass $Id
    }

    function Invoke-ClientWithStop {
        param([string]$Id, [ValidateSet("TERM", "KILL")][string]$Mode)
        if (-not (& $isSelected $Id)) { return }
        $process = Start-Client -Id $Id
        $entry = $nodeProcesses.GetEnumerator() | Where-Object { $_.Value.Id -eq $process.Id }
        $clientName = $entry.Key
        try {
            Wait-Log -Name $clientName -Pattern "scenario $Id armed node=" -Process $process -Attempts 900
            $line = Get-Content -LiteralPath (Join-Path $logDir "$clientName.log") |
                Select-String -Pattern "scenario $Id armed node=" -SimpleMatch | Select-Object -Last 1
            $node = $line.Line.Substring($line.Line.LastIndexOf("node=") + 5).Trim()
            Stop-ZoneNode -Name $node -Mode $Mode
            if (-not (Complete-Client -Process $process -Name $clientName)) {
                Add-Failure $Id "client verdict failed after stop"
            }
            if ($node -eq "zone-node-2") {
                Start-Zone -Name "zone-node-2" -ConfigName "zone-node-crash-replacement"
            } else {
                Start-Zone -Name $node
            }
        } catch {
            Add-Failure $Id $_.Exception.Message
        }
    }

    try {
        Set-Location $SampleDir
        $ports = @(Get-ZlinkSampleApplicationPorts -Language $Language -Count 9)
        $mesh1, $mesh2, $replacementMesh, $opsStream, $opsMesh, $gatewayStream,
            $gatewayMesh, $spareMesh, $previewPort = $ports
        $redis = Start-ZlinkSampleRedis "zlink-redis-$languageKey-sample-zoneworld" `
            -Language $Language
        $redisContainerId = $redis.ContainerId
        $redisEndpoint = $redis.Endpoint
        $redisKeyPrefix = "zoneworld:${languageKey}:$([IO.Path]::GetFileName($runDir)):${PID}:"

        $disableZoneBots = [bool]$B8Child
        Write-ServerConfig "zone-node-1" "zone" "zone-node-1" $mesh1 0 $false $disableZoneBots $false "*" | Out-Null
        Write-ServerConfig "zone-node-2" "zone" "zone-node-2" $mesh2 0 $false $disableZoneBots | Out-Null
        Write-ServerConfig "zone-node-3" "zone" "zone-node-3" $spareMesh 0 $true $true $true | Out-Null
        Write-ServerConfig "zone-node-replacement" "zone" "zone-node-2" $replacementMesh | Out-Null
        Write-ServerConfig "zone-node-crash-replacement" "zone" "zone-node-2" $replacementMesh 0 $false $true $true | Out-Null
        Write-ServerConfig "ops" "ops" "ops" $opsMesh $opsStream | Out-Null
        Write-ServerConfig "gateway" "gateway" "gateway" $gatewayMesh $gatewayStream | Out-Null

        $sourceExtension = if ($Language -eq "Java") { "*.java" } else { "*.kt" }
        $fixedPlacement = Get-ChildItem Server, Shared -Recurse -File -Filter $sourceExtension |
            Select-String -Pattern 'ZoneWorldSpec\.(zonesOf|nodeOf)|setRoutingId\(|\bzn[12]\b'
        if ($fixedPlacement) {
            throw "fixed placement/routing id found in $Language ZoneWorld"
        }

        Write-Output "==> build"
        Push-Location (Join-Path $SampleDir "../../..")
        try {
            $frameworkTasks = @(
                "--no-daemon", "--no-parallel", "--max-workers=1",
                ":zlink-framework-core:jar",
                ":zlink-framework-spring-boot-starter:jar",
                ":zlink-framework-locations-redis:jar",
                ":zlink-stream-connector:jar"
            )
            if ($Language -eq "Kotlin") { $frameworkTasks += ":zlink-framework-kotlin:jar" }
            $frameworkTasks += "--quiet"
            Invoke-ZlinkSampleGradleBuild -GradleExecutable $gradle -Arguments $frameworkTasks
        } finally {
            Pop-Location
        }
        Invoke-ZlinkSampleGradleBuild -GradleExecutable $gradle -Arguments @(
            "--settings-file", "standalone.settings.gradle.kts", "--no-daemon",
            "--no-parallel", "--max-workers=1", ":Server:installDist",
            ":Client:installDist", "--quiet")

        if ($B8Child) {
            $proxyScript = if ($Language -eq "Java") {
                Join-Path $SampleDir "Support/session_route_block_proxy.py"
            } else {
                Join-Path $SampleDir "../../java/ZoneWorld/Support/session_route_block_proxy.py"
            }
            $python = Resolve-ZlinkZoneWorldPython
            $pythonProcessName = [IO.Path]::GetFileNameWithoutExtension($python)
            foreach ($spec in @(
                    @{ Name = "zone-node-1"; Port = $mesh1 },
                    @{ Name = "zone-node-2"; Port = $mesh2 },
                    @{ Name = "gateway"; Port = $gatewayMesh })) {
                $proxyName = "proxy-$($spec.Name)"
                $proxy = Start-LoggedProcess -Name $proxyName -FilePath $python `
                    -ExpectedLeafProcessNames $pythonProcessName -Arguments @(
                    $proxyScript, "--listen-host", "127.0.0.1", "--listen-port", $spec.Port,
                    "--target-host", "127.0.0.2", "--target-port", $spec.Port,
                    "--arm-file", (Join-Path $runDir "b8-block-command-44"))
                Wait-Log -Name $proxyName -Pattern "proxy-ready" -Process $proxy
            }
        }

        Write-Output "==> topology"
        $ops = Start-LoggedProcess -Name "ops" -FilePath $serverBin `
            -Arguments @("--config", (Join-Path $configDir "ops.properties"))
        Wait-Log -Name "ops" -Pattern "ZLINK_FRAMEWORK_READY" -Process $ops -Attempts 900
        if ((& $isSelected "ZW-G2") -and -not $G4Child) {
            Start-Zone "zone-node-2"
            Start-Zone "zone-node-1"
        } else {
            Start-Zone "zone-node-1"
            Start-Zone "zone-node-2"
        }
        $rid1 = Get-RoutingId "zone-node-1"
        $rid2 = Get-RoutingId "zone-node-2"
        $gateway = Start-LoggedProcess -Name "gateway" -FilePath $serverBin `
            -Arguments @("--config", (Join-Path $configDir "gateway.properties"))
        Wait-Log -Name "gateway" -Pattern "ZLINK_FRAMEWORK_READY" -Process $gateway -Attempts 900
        Start-Zone -Name "zone-node-3" -WaitForStatusReport $false

        if ($g4Proven) { Add-Pass "ZW-G4" }
        if ($b8Proven) { Add-Pass "ZW-B8" }

        if ($B8Child) {
            $client = Start-Client "ZW-B8"
            $clientName = ($nodeProcesses.GetEnumerator() |
                Where-Object { $_.Value.Id -eq $client.Id }).Key
            Wait-Log -Name $clientName -Pattern "scenario ZW-B8 armed actor=" `
                -Process $client -Attempts 900
            $armedLine = Get-Content -LiteralPath (Join-Path $logDir "$clientName.log") |
                Select-String -Pattern "scenario ZW-B8 armed actor=" -SimpleMatch |
                Select-Object -Last 1 -ExpandProperty Line
            $actor = [regex]::Match($armedLine, 'actor=([^ ]+)').Groups[1].Value
            $target = [regex]::Match($armedLine, 'target=([^ ]+)').Groups[1].Value
            Set-ZlinkSampleUtf8File -Path (Join-Path $runDir "b8-block-command-44") -Value @()
            $proxyLogs = @(Get-ChildItem $logDir -Filter "proxy-*.log" | Select-Object -ExpandProperty FullName)
            $zoneLogs = @(
                (Join-Path $logDir "zone-node-1.log"),
                (Join-Path $logDir "zone-node-2.log"))
            for ($attempt = 0; $attempt -lt 600 -and
                    -not (Select-String -Path $proxyLogs -Pattern "blocked-command-44" -SimpleMatch); $attempt++) {
                if ($client.HasExited) { break }
                Start-Sleep -Milliseconds 100
            }
            if (-not (Select-String -Path $proxyLogs -Pattern "blocked-command-44" -SimpleMatch)) {
                throw "scenario ZW-B8 failed: fault proxy did not intercept command 44"
            }
            $commitPattern = "zone actor joined zone=$target actor=$actor "
            for ($attempt = 0; $attempt -lt 600 -and
                    -not (Select-String -Path $zoneLogs -Pattern $commitPattern -SimpleMatch); $attempt++) {
                if ($client.HasExited) { break }
                Start-Sleep -Milliseconds 100
            }
            if (-not (Select-String -Path $zoneLogs -Pattern $commitPattern -SimpleMatch)) {
                throw "scenario ZW-B8 failed: target relocation commit was not observed"
            }
            Remove-Item -LiteralPath (Join-Path $runDir "b8-block-command-44") -Force
            if (-not (Complete-Client -Process $client -Name $clientName)) {
                throw "scenario ZW-B8 failed: post-boundary reconnect did not rebind the Actor"
            }
            Add-Pass "ZW-B8"
            return
        }

        if ($G4Child) {
            $old = $rid2
            $client = Start-Client "ZW-G4"
            $clientName = ($nodeProcesses.GetEnumerator() |
                Where-Object { $_.Value.Id -eq $client.Id }).Key
            Wait-Log -Name $clientName -Pattern "scenario ZW-G4 armed node=zone-node-2" `
                -Process $client -Attempts 900
            Wait-Log -Name "zone-node-2" -Pattern "crash-boundary join pending" -Attempts 900
            Stop-ZoneNode "zone-node-2" "KILL"
            if (-not (Complete-Client -Process $client -Name $clientName)) {
                throw "scenario ZW-G4 failed"
            }
            Start-Zone "zone-node-2" "zone-node-crash-replacement"
            $new = Get-RoutingId "zone-node-2"
            if (-not (Test-ZoneRoutingId $new) -or $new -eq $old -or
                    -not (Invoke-Client "ZW-G4-fresh") -or
                    -not (Test-LogContains -Path (Join-Path $logDir "client.log") `
                        -Pattern "scenario ZW-G4-fresh owner=$new ")) {
                throw "scenario ZW-G4 fresh placement failed"
            }
            Add-Pass "ZW-G4"
            return
        }

        if (& $isSelected "ZW-G1") {
            if ((Test-ZoneRoutingId $rid1) -and (Test-ZoneRoutingId $rid2) -and $rid1 -ne $rid2) {
                Add-Pass "ZW-G1"
            } else { Add-Failure "ZW-G1" "noncanonical or duplicate RID" }
        }
        if (& $isSelected "ZW-G2") {
            if (Test-ZoneRoutingId $rid2) { Add-Pass "ZW-G2-rid" } else { Add-Failure "ZW-G2" "reverse-start RID invalid" }
        }
        if (& $isSelected "ZW-G5") { Add-Pass "ZW-G5" }

        foreach ($id in @(
                "ZW-A1", "ZW-A2", "ZW-A3", "ZW-A4", "ZW-A5",
                "ZW-B1", "ZW-B2", "ZW-B3", "ZW-B5", "ZW-B6", "ZW-B7",
                "ZW-C1", "ZW-C4", "ZW-D1", "ZW-E1", "ZW-E2", "ZW-E3",
                "ZW-E4", "ZW-E6", "ZW-F1", "ZW-F3", "ZW-F4")) {
            if ((& $isSelected $id) -and -not (Invoke-Client $id)) {
                Add-Failure $id "client-visible scenario failed; inspect client/role logs"
            }
        }
        if ((& $isSelected "ZW-G2") -and -not (Invoke-Client "ZW-G2")) {
            Add-Failure "ZW-G2" "reverse-start operations failed"
        }

        Invoke-ClientWithStop "ZW-B4" "KILL"
        Invoke-ClientWithStop "ZW-C2" "TERM"
        Invoke-ClientWithStop "ZW-C3" "KILL"

        if (& $isSelected "ZW-E5") {
            if (-not (Invoke-Client "ZW-E5-arm")) {
                Add-Failure "ZW-E5-arm" "could not store maintenance"
            }
            Stop-ZoneNode "zone-node-2" "KILL"
            try {
                Start-Zone "zone-node-2" "zone-node-crash-replacement"
                if (-not (Invoke-Client "ZW-E5")) {
                    Add-Failure "ZW-E5" "maintenance not restored"
                }
            } catch { Add-Failure "ZW-E5" "replacement did not reach topology ready" }
        }

        Test-AllLogs "ZW-D1-subscribers" "fanout subscriber received announcement" `
            @("zone-node-1.log", "zone-node-2.log")
        Test-AllLogs "ZW-D1-spots" "zone spot: announcement delivered" `
            @("zone-node-1.log", "zone-node-2.log")
        if (& $isSelected "ZW-D2") {
            if (Test-LogContains (Join-Path $logDir "zone-node-3.log") `
                    "fanout subscriber received announcement") { Add-Pass "ZW-D2" }
            else { Add-Failure "ZW-D2" "third subscriber missed publish" }
        }

        $zoneLogs = @(
            (Join-Path $logDir "zone-node-1.log"),
            (Join-Path $logDir "zone-node-2.log"))
        if (& $isSelected "ZW-F1") {
            $bots = @(Select-String -Path $zoneLogs -Pattern 'bot spawned\. bot=([a-z0-9-]+)' |
                ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique).Count
            if ($bots -eq 8) { Add-Pass "ZW-F1-population" }
            else { Add-Failure "ZW-F1-population" "bot roster count=$bots" }
        }
        if (& $isSelected "ZW-F2") {
            $crossed = $false
            for ($attempt = 0; $attempt -lt 300 -and -not $crossed; $attempt++) {
                foreach ($sourceIndex in 0, 1) {
                    $targetIndex = 1 - $sourceIndex
                    $botIds = Select-String -Path $zoneLogs[$sourceIndex] `
                        -Pattern 'player=(bot-[^,]+), bot=true, initial=false' |
                        ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique
                    foreach ($bot in $botIds) {
                        if (Test-LogContains $zoneLogs[$targetIndex] "player=$bot, bot=true") {
                            $crossed = $true
                            break
                        }
                    }
                }
                if (-not $crossed) { Start-Sleep -Milliseconds 100 }
            }
            if ($crossed) { Add-Pass "ZW-F2" } else { Add-Failure "ZW-F2" "no correlated cross-owner bot handoff" }
        }
        if (& $isSelected "ZW-F4") {
            if (Select-String -Path $zoneLogs -Pattern "No current session binding exists for actor 'bot-" -SimpleMatch) {
                Add-Failure "ZW-F4-no-push" "push attempted to bot"
            } else { Add-Pass "ZW-F4-no-push" }
        }

        $clientLog = Join-Path $logDir "client.log"
        if (& $isSelected "ZW-B5") {
            $line = Get-Content $clientLog | Select-String "message-follow-one-way completed" | Select-Object -Last 1
            $actor = if ($line) { [regex]::Match($line.Line, 'actor=([^ ]+)').Groups[1].Value } else { "" }
            $hits = @(Select-String -Path $zoneLogs -SimpleMatch "message-follow probe one-way handled. actor=$actor,").Count
            if ($actor -and $hits -eq 1) { Add-Pass "ZW-B5" } else { Add-Failure "ZW-B5" "one-way exact-once handler hits=$hits" }
        }
        if (& $isSelected "ZW-B6") {
            $line = Get-Content $clientLog | Select-String "message-follow-request completed" | Select-Object -Last 1
            $actor = if ($line) { [regex]::Match($line.Line, 'actor=([^ ]+)').Groups[1].Value } else { "" }
            $request = if ($line) { [regex]::Match($line.Line, 'request=([^ ]+)').Groups[1].Value } else { "" }
            $hits = @(Select-String -Path $zoneLogs -SimpleMatch "message-follow probe handled. actor=$actor, probe=$request,").Count
            if ($actor -and $request -and $hits -eq 1) { Add-Pass "ZW-B6" } else { Add-Failure "ZW-B6" "request exact-once handler hits=$hits" }
        }

        if (& $isSelected "ZW-G3") {
            $old = $rid2
            Stop-ZoneNode "zone-node-2" "TERM"
            $replacement = Start-LoggedProcess -Name "zone-node-replacement" -FilePath $serverBin `
                -Arguments @("--config", (Join-Path $configDir "zone-node-crash-replacement.properties"))
            try {
                Wait-Log -Name "zone-node-replacement" -Pattern "topology=ready" `
                    -Process $replacement -Attempts 900
                $new = Get-RoutingId "zone-node-replacement"
                if ((Test-ZoneRoutingId $new) -and $new -ne $old -and
                        (Invoke-Client "ZW-G3-fresh") -and
                        (Test-LogContains $clientLog "scenario ZW-G3-fresh owner=$new ")) {
                    Add-Pass "ZW-G3"
                } else { Add-Failure "ZW-G3" "replacement RID/fresh object placement failed" }
            } catch { Add-Failure "ZW-G3" "replacement did not reach topology ready" }
        }

        if ($Scenario -eq "all") {
            $passed = @{}
            foreach ($line in @(Get-Content $clientLog, $runnerLog -ErrorAction SilentlyContinue)) {
                if ($line -match '^scenario ([A-Za-z0-9-]+) passed$') { $passed[$Matches[1]] = $true }
            }
            function Write-Phase {
                param([string]$Marker, [string[]]$Ids)
                foreach ($id in $Ids) {
                    if (-not $passed.ContainsKey($id)) {
                        [Console]::Error.WriteLine("!! $Marker withheld: $id did not pass")
                        $state.Status = 1
                        return
                    }
                }
                Write-Output $Marker
            }
            Write-Phase "zoneworld-relocation=completed" @("ZW-B2", "ZW-B3", "ZW-B5", "ZW-B6", "ZW-B7", "ZW-B8", "ZW-F2")
            Write-Phase "zoneworld-border-sync=completed" @("ZW-B1", "ZW-B4")
            Write-Phase "zoneworld-ops-observe=completed" @("ZW-C1", "ZW-C2", "ZW-C3", "ZW-C4")
            Write-Phase "zoneworld-ops-announce=completed" @("ZW-D1", "ZW-D1-subscribers", "ZW-D1-spots", "ZW-D2")
            Write-Phase "zoneworld-ops-maintenance=completed" @("ZW-E1", "ZW-E2", "ZW-E3", "ZW-E4", "ZW-E5", "ZW-E6")
            Write-Phase "zoneworld=completed" @(
                "ZW-A1", "ZW-A2", "ZW-A3", "ZW-A4", "ZW-A5",
                "ZW-B1", "ZW-B2", "ZW-B3", "ZW-B4", "ZW-B5", "ZW-B6", "ZW-B7", "ZW-B8",
                "ZW-C1", "ZW-C2", "ZW-C3", "ZW-C4", "ZW-D1", "ZW-D1-subscribers", "ZW-D1-spots", "ZW-D2",
                "ZW-E1", "ZW-E2", "ZW-E3", "ZW-E4", "ZW-E5", "ZW-E5-arm", "ZW-E6",
                "ZW-F1", "ZW-F1-population", "ZW-F2", "ZW-F3", "ZW-F4", "ZW-F4-no-push",
                "ZW-G1", "ZW-G2-rid", "ZW-G2", "ZW-G3", "ZW-G4", "ZW-G5")
        }
        Write-Output "==> logs: $logDir"
    } catch {
        $state.Status = 1
        [Console]::Error.WriteLine($_.Exception.Message)
        Write-FailureLogs
    } finally {
        $cleanupFailures = [Collections.Generic.List[string]]::new()
        try { Stop-AllProcesses } catch { $cleanupFailures.Add($_.Exception.Message) }
        if ($redisContainerId) {
            try { Remove-ZlinkSampleRedis $redisContainerId } catch {
                $cleanupFailures.Add($_.Exception.Message)
            }
        }
        if ($env:ZLINK_SAMPLE_KEEP_RUN_DIR -eq "1" -or $cleanupFailures.Count -ne 0) {
            Write-Output "runDir=$runDir"
        } else {
            try { Remove-Item -LiteralPath $runDir -Recurse -Force -ErrorAction Stop } catch {
                $cleanupFailures.Add("Could not remove ZoneWorld run directory: $($_.Exception.Message)")
            }
        }
        if ($cleanupFailures.Count -ne 0) {
            $state.Status = 1
            foreach ($failure in $cleanupFailures) {
                [Console]::Error.WriteLine("cleanup failed: $failure")
            }
        }
    }
    if ($state.Status -ne 0) { throw "$Language ZoneWorld sample failed." }
}

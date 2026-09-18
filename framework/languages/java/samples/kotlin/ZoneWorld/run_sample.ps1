param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RunnerArguments
)

Set-StrictMode -Version Latest
. "$PSScriptRoot/../../redis-common.ps1"
$ErrorActionPreference = "Stop"

$SampleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $SampleDir

$Scenario = "all"
$G4Child = $false
$B8Child = $false
foreach ($argument in $RunnerArguments) {
    if ($argument -eq "--g4-child") { $G4Child = $true }
    elseif ($argument -eq "--b8-child") { $B8Child = $true }
    elseif ($argument -in @("full", "all")) { $Scenario = "all" }
    else { $Scenario = $argument }
}
function Test-Scenario {
    param([Parameter(Mandatory = $true)][string]$Id)
    if ($Scenario -eq "all") { return $true }
    return $Id -in @($Scenario.Split(',', [System.StringSplitOptions]::RemoveEmptyEntries))
}

$RunDir = Join-Path ([IO.Path]::GetTempPath()) "zlink-zoneworld-kotlin-$PID-$([Guid]::NewGuid().ToString('N'))"
$LogDir = Join-Path $RunDir "logs"
$ConfigDir = Join-Path $RunDir "config"
New-Item -ItemType Directory -Force -Path $LogDir, $ConfigDir | Out-Null

$Gradle = if ($IsWindows) { Join-Path $SampleDir "../../gradlew.bat" } else { Join-Path $SampleDir "../../gradlew" }
$RootGradle = if ($IsWindows) { Join-Path $SampleDir "../../../gradlew.bat" } else { Join-Path $SampleDir "../../../gradlew" }
$ServerBin = Join-Path $SampleDir "Server/build/install/Server/bin/Server"
$ClientBin = Join-Path $SampleDir "Client/build/install/Client/bin/Client"
if ($IsWindows) { $ServerBin = "$ServerBin.bat"; $ClientBin = "$ClientBin.bat" }

$Processes = [System.Collections.Generic.List[System.Diagnostics.Process]]::new()
$NodeProcesses = @{}
$RedisContainer = $null
$ClientRunNumber = 0
$Status = 0
$RunSucceeded = $false
$G4Proven = $false
$B8Proven = $false

function Protect-ConfigFile {
    param([Parameter(Mandatory = $true)][string]$Path)
    if ($IsWindows) {
        $identity = [Security.Principal.WindowsIdentity]::GetCurrent().Name
        & icacls $Path /inheritance:r /grant:r "${identity}:(R,W)" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Could not restrict config file ACL: $Path" }
    } else {
        & chmod 0600 $Path
        if ($LASTEXITCODE -ne 0) { throw "Could not restrict config file mode: $Path" }
    }
}

function Get-CurrentLogPath {
    param([Parameter(Mandatory = $true)][string]$Name)
    return Join-Path $LogDir "$Name.log"
}

function Get-CurrentErrorLogPath {
    param([Parameter(Mandatory = $true)][string]$Name)
    return Join-Path $LogDir "$Name.err.log"
}

function Get-LogText {
    param([Parameter(Mandatory = $true)][string[]]$Names)
    return ($Names | ForEach-Object {
        $name = $_
        Get-ChildItem -LiteralPath $LogDir -File -Filter "$name*.log" -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -notlike "*.err.log" } |
            ForEach-Object { Get-Content -Raw -LiteralPath $_.FullName }
    }) -join [Environment]::NewLine
}

function Get-NextLogLine {
    param([Parameter(Mandatory = $true)][string]$Name)
    $path = Get-CurrentLogPath $Name
    if (-not (Test-Path -LiteralPath $path)) { return 1 }
    return @((Get-Content -LiteralPath $path)).Count + 1
}

function Start-ManagedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$ArchiveExisting
    )
    $outputPath = Get-CurrentLogPath $Name
    $errorPath = Get-CurrentErrorLogPath $Name
    if ($ArchiveExisting) {
        $suffix = [Guid]::NewGuid().ToString('N')
        if (Test-Path -LiteralPath $outputPath) {
            Move-Item -LiteralPath $outputPath -Destination (Join-Path $LogDir "$Name.$suffix.log")
        }
        if (Test-Path -LiteralPath $errorPath) {
            Move-Item -LiteralPath $errorPath -Destination (Join-Path $LogDir "$Name.$suffix.err.log")
        }
    }
    $argumentLine = ($Arguments | ForEach-Object { ConvertTo-ZlinkSampleProcessArgument $_ }) -join " "
    $process = Start-Process -FilePath $Executable -ArgumentList $argumentLine `
            -WorkingDirectory $SampleDir -NoNewWindow -RedirectStandardOutput $outputPath `
            -RedirectStandardError $errorPath -PassThru
    [void]$process.Handle
    $Processes.Add($process)
    return $process
}

function Start-Role {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )
    $process = Start-ManagedProcess $Name $Executable $Arguments -ArchiveExisting
    $NodeProcesses[$Name] = $process
    Write-Host "    started $Name pid=$($process.Id)"
    return $process
}

function Stop-Node {
    param([Parameter(Mandatory = $true)][string]$Name, [string]$Mode = "KILL")
    if (-not $NodeProcesses.ContainsKey($Name)) { return }
    $process = $NodeProcesses[$Name]
    if (-not $process.HasExited) { Stop-ZlinkSampleProcessTree -Process $process }
    $process.WaitForExit()
    $NodeProcesses.Remove($Name)
}

function Wait-Log {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [int]$FirstLine = 1,
        [int]$Attempts = 600
    )
    $path = Get-CurrentLogPath $Name
    for ($attempt = 0; $attempt -lt $Attempts; $attempt++) {
        if ((Test-Path -LiteralPath $path) -and
            (@(Get-Content -LiteralPath $path | Select-Object -Skip ($FirstLine - 1)) |
                Select-String -SimpleMatch $Pattern -Quiet)) { return $true }
        Start-Sleep -Milliseconds 100
    }
    [Console]::Error.WriteLine("Timed out waiting for '$Pattern' in $Name after line $FirstLine")
    if (Test-Path -LiteralPath $path) {
        Get-Content -LiteralPath $path -Tail 80 | ForEach-Object { [Console]::Error.WriteLine($_) }
    }
    return $false
}

function Wait-LogWhileRunning {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [int]$FirstLine,
        [Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process,
        [int]$Attempts = 600
    )
    $path = Get-CurrentLogPath $Name
    for ($attempt = 0; $attempt -lt $Attempts; $attempt++) {
        if ((Test-Path -LiteralPath $path) -and
            (@(Get-Content -LiteralPath $path | Select-Object -Skip ($FirstLine - 1)) |
                Select-String -SimpleMatch $Pattern -Quiet)) { return $true }
        if ($Process.HasExited) { return $false }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

function Wait-EvidenceWhileRunning {
    param(
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)][string[]]$Names,
        [int]$Attempts = 600
    )
    for ($attempt = 0; $attempt -lt $Attempts; $attempt++) {
        if ((Get-LogText $Names) -like "*$Pattern*") { return $true }
        if ($Process.HasExited) { return $false }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

# A ZoneNode never prints its own RID. The RID reaches an observer only through the Ops
# node status report (sample README 9.2-7, 9.3), so read it from ops.log and wait there for
# the incarnation that started at line $FirstLine of that log to report.
function Get-RoutingId {
    param(
        [Parameter(Mandatory = $true)][string]$NodeId,
        [int]$FirstLine = 1,
        [int]$Attempts = 900
    )
    if (-not (Wait-Log "ops" "node status observed. node=$NodeId, rid=zn-" $FirstLine $Attempts)) {
        return ""
    }
    $path = Get-CurrentLogPath "ops"
    $pattern = "node status observed\. node=$([regex]::Escape($NodeId)), rid=(zn-[0-9a-f-]+)"
    $stream = [IO.File]::Open(
        $path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $reader = [IO.StreamReader]::new($stream)
    $observed = ""
    try {
        $lineNumber = 0
        while ($null -ne ($line = $reader.ReadLine())) {
            $lineNumber++
            if ($lineNumber -lt $FirstLine) { continue }
            $match = [regex]::Match($line, $pattern)
            if ($match.Success) { $observed = $match.Groups[1].Value }
        }
    } finally {
        $reader.Dispose()
    }
    return $observed
}

function Test-ZoneRoutingId {
    param([AllowEmptyString()][string]$Value)
    return $Value -match '^zn-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$'
}

function Write-ServerConfig {
    param(
        [string]$Name, [string]$Role, [string]$Node, [int]$Mesh, [int]$Stream,
        [bool]$Subscriber = $false, [bool]$DisableBots = $false,
        [bool]$AllowEmpty = $false, [string]$Fault = ""
    )
    $bindHost = "127.0.0.1"
    $advertise = ""
    if ($B8Child -and $Role -ne "ops" -and -not $Subscriber) {
        $bindHost = "127.0.0.2"
        $advertise = "127.0.0.1"
    }
    $path = Join-Path $ConfigDir "$Name.properties"
    Set-ZlinkSampleUtf8File -Path $path -Value @(
        "sample.role=$Role",
        "sample.node-id=$Node",
        "sample.mesh-endpoint=tcp://${bindHost}:$Mesh",
        "sample.stream-endpoint=tcp://127.0.0.1:$Stream",
        "sample.redis-endpoint=$RedisEndpoint",
        "sample.redis-key-prefix=$RedisKeyPrefix",
        "sample.subscriber-only=$($Subscriber.ToString().ToLowerInvariant())",
        "sample.disable-bots=$($DisableBots.ToString().ToLowerInvariant())",
        "sample.allow-empty-zone-set=$($AllowEmpty.ToString().ToLowerInvariant())",
        "sample.fault-tick-zone=$Fault",
        "sample.mesh-advertise-host=$advertise"
    )
    Protect-ConfigFile $path
}

function Write-ClientConfig {
    param([Parameter(Mandatory = $true)][string]$Id)
    $safeId = $Id.Replace(',', '-')
    $path = Join-Path $ConfigDir "client-$safeId-$($script:ClientRunNumber).properties"
    $armFile = (Join-Path $RunDir "b8-block-command-44").Replace('\', '/')
    Set-ZlinkSampleUtf8File -Path $path -Value @(
        "sample.gateway-endpoint=tcp://127.0.0.1:$GatewayStream",
        "sample.ops-endpoint=tcp://127.0.0.1:$OpsStream",
        "sample.scenarios=$Id",
        "sample.stream-trace=$(if ($env:ZLINK_JAVA_STREAM_TRACE -eq '1') { 'true' } else { 'false' })",
        "sample.fault-arm-file=$armFile"
    )
    Protect-ConfigFile $path
    return $path
}

function Start-Client {
    param([Parameter(Mandatory = $true)][string]$Id)
    $script:ClientRunNumber++
    $name = "client-run-$($script:ClientRunNumber)"
    $config = Write-ClientConfig $Id
    $process = Start-ManagedProcess $name $ClientBin @("--config", $config)
    return [pscustomobject]@{ Name = $name; Process = $process }
}

function Complete-Client {
    param([Parameter(Mandatory = $true)]$Run)
    $Run.Process.WaitForExit()
    $Run.Process.Refresh()
    $outputPath = Get-CurrentLogPath $Run.Name
    $errorPath = Get-CurrentErrorLogPath $Run.Name
    if (Test-Path -LiteralPath $outputPath) {
        Get-Content -LiteralPath $outputPath | Tee-Object -FilePath $ClientLog -Append | Write-Host
    }
    if (Test-Path -LiteralPath $errorPath) {
        Get-Content -LiteralPath $errorPath | Tee-Object -FilePath $ClientErrorLog -Append |
            ForEach-Object { [Console]::Error.WriteLine($_) }
    }
    return $Run.Process.ExitCode -eq 0
}

function Invoke-Client {
    param([Parameter(Mandatory = $true)][string]$Id)
    $run = Start-Client $Id
    return Complete-Client $run
}

function Start-Zone {
    param([string]$Name)
    # Every restart is a replacement: same NodeId, a new RID on the node's own replacement
    # endpoint, ready with no zones. The stop kind does not change that, so the configuration
    # follows from the node name alone.
    $ConfigName = "$Name-replacement"
    $process = Start-Role $Name $ServerBin @("--config", (Join-Path $ConfigDir "$ConfigName.properties"))
    if (-not (Wait-LogWhileRunning $Name "topology=ready" 1 $process 900)) { return $false }
    return Wait-LogWhileRunning $Name "node status report submitted" 1 $process 900
}

function Add-Verdict {
    param([string]$Id, [bool]$Passed, [string]$Failure = "")
    $line = if ($Passed) { "scenario $Id passed" } else { "scenario $Id failed" }
    Add-Content -LiteralPath $RunnerLog -Value $line
    if ($Passed) { Write-Host $line }
    else {
        $script:Status = 1
        [Console]::Error.WriteLine($line)
        if ($Failure) { [Console]::Error.WriteLine("    $Failure") }
    }
}

function Invoke-ClientWithStop {
    param([string]$Id, [string]$Mode)
    if (-not (Test-Scenario $Id)) { return }
    $run = Start-Client $Id
    if (-not (Wait-LogWhileRunning $run.Name "scenario $Id armed node=" 1 $run.Process 900)) {
        [void](Complete-Client $run)
        Add-Verdict $Id $false "client did not arm"
        return
    }
    $armed = @(Get-Content -LiteralPath (Get-CurrentLogPath $run.Name) |
        Select-String -Pattern "scenario $([regex]::Escape($Id)) armed node=([^ ]+)")[-1]
    if ($null -eq $armed) { Add-Verdict $Id $false "client did not identify the node"; return }
    $node = $armed.Matches[0].Groups[1].Value
    Stop-Node $node $Mode
    if (-not (Complete-Client $run)) { Add-Verdict $Id $false "client verdict failed after stop" }
    if (-not (Start-Zone $node)) { Add-Verdict $Id $false "replacement did not reach topology ready" }
}

function Test-EveryLog {
    param([string[]]$Names, [string]$Pattern)
    foreach ($name in $Names) {
        if ((Get-LogText @($name)) -notlike "*$Pattern*") { return $false }
    }
    return $true
}

function Assert-Phase {
    param([string]$Marker, [string[]]$Ids)
    $verdicts = @()
    if (Test-Path -LiteralPath $ClientLog) { $verdicts += Get-Content -LiteralPath $ClientLog }
    if (Test-Path -LiteralPath $RunnerLog) { $verdicts += Get-Content -LiteralPath $RunnerLog }
    foreach ($id in $Ids) {
        if ($verdicts -notcontains "scenario $id passed") {
            [Console]::Error.WriteLine("!! $Marker withheld: $id did not pass")
            $script:Status = 1
            return
        }
    }
    Write-Host $Marker
}

function Invoke-IsolatedChild {
    param([string]$Name, [string[]]$Arguments)
    $powerShell = Get-ZlinkSampleSelfShellPath
    $child = Start-Process -FilePath $powerShell -ArgumentList ((@(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $PSCommandPath
    ) + $Arguments | ForEach-Object { ConvertTo-ZlinkSampleProcessArgument $_ }) -join " ") `
        -WorkingDirectory $SampleDir -NoNewWindow -PassThru
    [void]$child.Handle
    try {
        $child.WaitForExit()
        $childExitCode = $child.ExitCode
    } finally {
        Stop-ZlinkSampleProcessTree -Process $child
        $child.Dispose()
    }
    return $childExitCode -eq 0
}

try {
    if (-not $G4Child -and (Test-Scenario "ZW-G4")) {
        $G4Proven = Invoke-IsolatedChild "g4-child" @("--g4-child", "ZW-G4")
        if (-not $G4Proven) { [Console]::Error.WriteLine("scenario ZW-G4 blocked: isolated crash lane failed") }
        if ($Scenario -eq "ZW-G4") {
            if ($G4Proven) { $RunSucceeded = $true; exit 0 } else { exit 1 }
        }
    }
    if (-not $B8Child -and (Test-Scenario "ZW-B8")) {
        $B8Proven = Invoke-IsolatedChild "b8-child" @("--b8-child", "ZW-B8")
        if (-not $B8Proven) { [Console]::Error.WriteLine("scenario ZW-B8 blocked: isolated command-44 lane failed") }
        if ($Scenario -eq "ZW-B8") {
            if ($B8Proven) { $RunSucceeded = $true; exit 0 } else { exit 1 }
        }
    }

    $ports = @(Get-ZlinkSampleApplicationPorts -Language Kotlin -Count 10)
    $Mesh1 = $ports[0]; $Mesh2 = $ports[1]
    $ReplacementMesh1 = $ports[2]; $ReplacementMesh2 = $ports[3]
    $OpsStream = $ports[4]; $OpsMesh = $ports[5]; $GatewayStream = $ports[6]
    $GatewayMesh = $ports[7]; $SpareMesh = $ports[8]

    $redis = Start-ZlinkSampleRedis "zlink-redis-kotlin-sample-zoneworld" `
        $(if ($env:ZLINK_REDIS_IMAGE) { $env:ZLINK_REDIS_IMAGE } else { "redis:7.2-alpine" }) -Language Kotlin
    $RedisContainer = $redis.ContainerId
    $RedisEndpoint = $redis.Endpoint
    $RedisKeyPrefix = "zoneworld:kotlin:$([IO.Path]::GetFileName($RunDir)):${PID}:"

    Write-ServerConfig "zone-node-1" "zone" "zone-node-1" $Mesh1 0 $false $false $false "*"
    Write-ServerConfig "zone-node-2" "zone" "zone-node-2" $Mesh2 0
    Write-ServerConfig "zone-node-3" "zone" "zone-node-3" $SpareMesh 0 $true $true $true
    # One replacement configuration per node, and it is the only configuration a restart uses.
    # A stopped owner's zones stay with the incarnation that owned them, so a restarted node
    # reaches ready with no zones and spawns no bots, whether it was stopped or killed.
    Write-ServerConfig "zone-node-1-replacement" "zone" "zone-node-1" $ReplacementMesh1 0 $false $true $true
    Write-ServerConfig "zone-node-2-replacement" "zone" "zone-node-2" $ReplacementMesh2 0 $false $true $true
    Write-ServerConfig "ops" "ops" "ops" $OpsMesh $OpsStream
    Write-ServerConfig "gateway" "gateway" "gateway" $GatewayMesh $GatewayStream

    Assert-ZlinkSampleSourcePolicy -Path "Server", "Shared" -Extension ".kt" `
        -Pattern 'ZoneWorldSpec\.(zonesOf|nodeOf)|setRoutingId\(|\bzn[12]\b' `
        -Message "fixed placement/routing id found in Kotlin ZoneWorld"

    Write-Host "==> build"
    Push-Location "../../.."
    try {
        Invoke-ZlinkSampleGradleBuild -GradleExecutable $RootGradle -Arguments @(
            "--no-daemon", "--no-parallel", "--max-workers=1",
            ":zlink-framework-core:jar", ":zlink-framework-spring-boot-starter:jar",
            ":zlink-framework-locations-redis:jar", ":zlink-framework-kotlin:jar", ":zlink-stream-connector:jar", "--quiet")
    } finally { Pop-Location }
    Invoke-ZlinkSampleGradleBuild -GradleExecutable $Gradle -SettingsPath "standalone.settings.gradle.kts" -Arguments @(
        "--no-daemon", "--no-parallel", "--max-workers=1", ":Server:installDist", ":Client:installDist", "--quiet")

    if ($B8Child) {
        $python = Get-Command python.exe, python, py.exe, py -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -eq $python) { throw "Python is required for the ZW-B8 fault proxy." }
        foreach ($proxy in @(
            @{ Name = "proxy-zone-node-1"; Port = $Mesh1 },
            @{ Name = "proxy-zone-node-2"; Port = $Mesh2 },
            @{ Name = "proxy-gateway"; Port = $GatewayMesh })) {
            $arguments = @()
            if ($python.Name -in @("py.exe", "py")) { $arguments += "-3" }
            $arguments += @(
                (Join-Path $SampleDir "../../java/ZoneWorld/Support/session_route_block_proxy.py"),
                "--listen-host", "127.0.0.1", "--listen-port", "$($proxy.Port)",
                "--target-host", "127.0.0.2", "--target-port", "$($proxy.Port)",
                "--arm-file", (Join-Path $RunDir "b8-block-command-44"))
            Start-Role $proxy.Name $python.Source $arguments | Out-Null
            if (-not (Wait-Log $proxy.Name "proxy-ready")) { throw "$($proxy.Name) did not become ready" }
        }
    }

    Write-Host "==> topology"
    Start-Role "ops" $ServerBin @("--config", (Join-Path $ConfigDir "ops.properties")) | Out-Null
    if (-not (Wait-Log "ops" "ZLINK_FRAMEWORK_READY" 1 900)) { throw "ops did not become ready" }
    if ((Test-Scenario "ZW-G2") -and -not $G4Child) {
        Start-Role "zone-node-2" $ServerBin @("--config", (Join-Path $ConfigDir "zone-node-2.properties")) | Out-Null
        if (-not (Wait-Log "zone-node-2" "topology=ready" 1 900)) { throw "zone-node-2 did not become ready" }
        Start-Role "zone-node-1" $ServerBin @("--config", (Join-Path $ConfigDir "zone-node-1.properties")) | Out-Null
    } else {
        Start-Role "zone-node-1" $ServerBin @("--config", (Join-Path $ConfigDir "zone-node-1.properties")) | Out-Null
        if (-not (Wait-Log "zone-node-1" "topology=ready" 1 900)) { throw "zone-node-1 did not become ready" }
        Start-Role "zone-node-2" $ServerBin @("--config", (Join-Path $ConfigDir "zone-node-2.properties")) | Out-Null
    }
    foreach ($name in @("zone-node-1", "zone-node-2")) {
        if (-not (Wait-Log $name "topology=ready" 1 900)) { throw "$name did not reach topology ready" }
        if (-not (Wait-Log $name "node status report submitted" 1 900)) { throw "$name did not submit node status" }
    }
    $Rid1 = Get-RoutingId "zone-node-1"
    $Rid2 = Get-RoutingId "zone-node-2"
    Start-Role "gateway" $ServerBin @("--config", (Join-Path $ConfigDir "gateway.properties")) | Out-Null
    if (-not (Wait-Log "gateway" "ZLINK_FRAMEWORK_READY" 1 900)) { throw "gateway did not become ready" }
    Start-Role "zone-node-3" $ServerBin @("--config", (Join-Path $ConfigDir "zone-node-3.properties")) | Out-Null
    if (-not (Wait-Log "zone-node-3" "topology=ready" 1 900)) { throw "zone-node-3 did not become ready" }

    $ClientLog = Join-Path $LogDir "client.log"
    $ClientErrorLog = Join-Path $LogDir "client.err.log"
    $RunnerLog = Join-Path $LogDir "runner.log"
    New-Item -ItemType File -Force -Path $ClientLog, $ClientErrorLog, $RunnerLog | Out-Null

    if ($G4Proven) { Add-Verdict "ZW-G4" $true }
    if ($B8Proven) { Add-Verdict "ZW-B8" $true }

    if ($B8Child) {
        $run = Start-Client "ZW-B8"
        if (-not (Wait-LogWhileRunning $run.Name "scenario ZW-B8 armed actor=" 1 $run.Process 900)) {
            [void](Complete-Client $run); throw "ZW-B8 client did not arm"
        }
        $armed = @(Get-Content -LiteralPath (Get-CurrentLogPath $run.Name) |
            Select-String -Pattern 'scenario ZW-B8 armed actor=([^ ]+) target=([^ ]+)')[-1]
        if ($null -eq $armed) { throw "ZW-B8 did not identify its actor and target" }
        $actor = $armed.Matches[0].Groups[1].Value
        $target = $armed.Matches[0].Groups[2].Value
        New-Item -ItemType File -Force -Path (Join-Path $RunDir "b8-block-command-44") | Out-Null
        if (-not (Wait-EvidenceWhileRunning "blocked-command-44" $run.Process @(
            "proxy-zone-node-1", "proxy-zone-node-2", "proxy-gateway") 600)) {
            [void](Complete-Client $run)
            throw "scenario ZW-B8 failed: precondition unmet: fault proxy did not intercept command 44"
        }
        $commitPattern = "zone actor joined zone=$target actor=$actor "
        if (-not (Wait-EvidenceWhileRunning $commitPattern $run.Process @("zone-node-1", "zone-node-2") 600)) {
            [void](Complete-Client $run)
            throw "scenario ZW-B8 failed: precondition unmet: target relocation commit was not observed for actor $actor in $target"
        }
        Remove-Item -Force -LiteralPath (Join-Path $RunDir "b8-block-command-44") -ErrorAction SilentlyContinue
        if (-not (Complete-Client $run)) {
            throw "scenario ZW-B8 failed: post-boundary reconnect did not rebind the existing relocated Actor"
        }
        Add-Verdict "ZW-B8" $true
        $RunSucceeded = $true
        exit 0
    }

    if ($G4Child) {
        $old = $Rid2
        $targetFirst = Get-NextLogLine "zone-node-2"
        $run = Start-Client "ZW-G4"
        if (-not (Wait-LogWhileRunning $run.Name "scenario ZW-G4 armed node=zone-node-2" 1 $run.Process 900)) {
            [void](Complete-Client $run); throw "scenario ZW-G4 failed"
        }
        if (-not (Wait-Log "zone-node-2" "crash-boundary join pending" $targetFirst 900)) { throw "scenario ZW-G4 failed" }
        Stop-Node "zone-node-2" "KILL"
        if (-not (Complete-Client $run)) { throw "scenario ZW-G4 failed" }
        $opsFirst = Get-NextLogLine "ops"
        if (-not (Start-Zone "zone-node-2")) { throw "scenario ZW-G4 failed" }
        $new = Get-RoutingId "zone-node-2" $opsFirst
        if (-not (Test-ZoneRoutingId $new) -or $new -eq $old) { throw "scenario ZW-G4 failed" }
        if (-not (Invoke-Client "ZW-G4-fresh") -or
            -not (Select-String -LiteralPath $ClientLog -SimpleMatch "scenario ZW-G4-fresh owner=$new " -Quiet)) {
            throw "scenario ZW-G4 failed"
        }
        Add-Verdict "ZW-G4" $true
        $RunSucceeded = $true
        exit 0
    }

    if (Test-Scenario "ZW-G1") {
        Add-Verdict "ZW-G1" ((Test-ZoneRoutingId $Rid1) -and (Test-ZoneRoutingId $Rid2) -and $Rid1 -ne $Rid2) "noncanonical or duplicate RID"
    }
    if (Test-Scenario "ZW-G2") { Add-Verdict "ZW-G2-rid" (Test-ZoneRoutingId $Rid2) "reverse-start RID invalid" }
    if (Test-Scenario "ZW-G5") { Add-Verdict "ZW-G5" $true }

    $clientIds = @(
        "ZW-A1", "ZW-A2", "ZW-A3", "ZW-A4", "ZW-A5", "ZW-B1", "ZW-B2", "ZW-B3",
        "ZW-B5", "ZW-B6", "ZW-B7", "ZW-C1", "ZW-C4", "ZW-D1", "ZW-E1", "ZW-E2",
        "ZW-E3", "ZW-E4", "ZW-E6", "ZW-F1", "ZW-F3", "ZW-F4")
    foreach ($id in $clientIds) {
        if ((Test-Scenario $id) -and -not (Invoke-Client $id)) {
            Add-Verdict $id $false "client-visible scenario failed; inspect client/role logs"
        }
    }
    if ((Test-Scenario "ZW-G2") -and -not (Invoke-Client "ZW-G2")) {
        Add-Verdict "ZW-G2" $false "reverse-start operations failed"
    }

    Invoke-ClientWithStop "ZW-B4" "KILL"
    Invoke-ClientWithStop "ZW-C2" "TERM"
    Invoke-ClientWithStop "ZW-C3" "KILL"

    if (Test-Scenario "ZW-E5") {
        if (-not (Invoke-Client "ZW-E5-arm")) { Add-Verdict "ZW-E5-arm" $false "could not store maintenance" }
        $run = Start-Client "ZW-E5"
        if (-not (Wait-LogWhileRunning $run.Name "scenario ZW-E5 restore armed" 1 $run.Process 900)) {
            [void](Complete-Client $run); Add-Verdict "ZW-E5" $false "client did not arm restart observation"
        } else {
            Stop-Node "zone-node-2" "KILL"
            if (-not (Wait-LogWhileRunning $run.Name "scenario ZW-E5 replacement waiting" 1 $run.Process 900)) {
                [void](Complete-Client $run); Add-Verdict "ZW-E5" $false "client did not observe the stopped node"
            } elseif (-not (Start-Zone "zone-node-2")) {
                Add-Verdict "ZW-E5" $false "replacement did not reach topology ready"
            } elseif (-not (Complete-Client $run)) {
                Add-Verdict "ZW-E5" $false "maintenance not restored"
            }
        }
    }

    if (Test-Scenario "ZW-D1-subscribers") {
        Add-Verdict "ZW-D1-subscribers" (Test-EveryLog @("zone-node-1", "zone-node-2") "fanout subscriber received announcement") "missing 'fanout subscriber received announcement'"
    }
    if (Test-Scenario "ZW-D1-spots") {
        Add-Verdict "ZW-D1-spots" (Test-EveryLog @("zone-node-1", "zone-node-2") "zone spot: announcement delivered") "missing 'zone spot: announcement delivered'"
    }
    if (Test-Scenario "ZW-D2") {
        Add-Verdict "ZW-D2" ((Get-LogText @("zone-node-3")) -like "*fanout subscriber received announcement*") "third subscriber missed publish"
    }

    $zoneLogs = Get-LogText @("zone-node-1", "zone-node-2")
    $zoneLines = @($zoneLogs -split "`r`n|`n|`r")
    if (Test-Scenario "ZW-F1") {
        $bots = @($zoneLines | Select-String -Pattern 'bot spawned\. bot=([a-z0-9-]+)' |
            ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique)
        Add-Verdict "ZW-F1-population" ($bots.Count -eq 8) "bot roster count=$($bots.Count)"
    }
    if (Test-Scenario "ZW-F2") {
        $crossed = $false
        for ($attempt = 0; $attempt -lt 300 -and -not $crossed; $attempt++) {
            $node1 = Get-LogText @("zone-node-1")
            $node2 = Get-LogText @("zone-node-2")
            $node1Bots = @($node1 -split "`r`n|`n|`r" | Select-String -Pattern 'player=(bot-[^,]+), bot=true, initial=false' |
                ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique)
            $node2Bots = @($node2 -split "`r`n|`n|`r" | Select-String -Pattern 'player=(bot-[^,]+), bot=true, initial=false' |
                ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique)
            $crossed = @($node1Bots | Where-Object { $node2 -like "*player=$_, bot=true*" }).Count -gt 0 -or
                @($node2Bots | Where-Object { $node1 -like "*player=$_, bot=true*" }).Count -gt 0
            if (-not $crossed) { Start-Sleep -Milliseconds 100 }
        }
        Add-Verdict "ZW-F2" $crossed "no correlated cross-owner bot handoff"
    }
    if (Test-Scenario "ZW-F4") {
        Add-Verdict "ZW-F4-no-push" ($zoneLogs -notlike "*No current session binding exists for actor 'bot-*") "push attempted to bot"
    }

    if (Test-Scenario "ZW-B5") {
        $line = @(Get-Content -LiteralPath $ClientLog | Select-String -SimpleMatch "message-follow-one-way completed")[-1]
        $actor = if ($null -ne $line -and $line.Line -match 'actor=([^ ]+)') { $matches[1] } else { "" }
        $hits = if ($actor) { @($zoneLines | Where-Object { $_ -like "*message-follow probe one-way handled. actor=$actor,*" }).Count } else { 0 }
        Add-Verdict "ZW-B5" ([bool]$actor -and $hits -eq 1) "one-way exact-once handler hits=$hits"
    }
    if (Test-Scenario "ZW-B6") {
        $line = @(Get-Content -LiteralPath $ClientLog | Select-String -SimpleMatch "message-follow-request completed")[-1]
        $actor = if ($null -ne $line -and $line.Line -match 'actor=([^ ]+)') { $matches[1] } else { "" }
        $request = if ($null -ne $line -and $line.Line -match 'request=([^ ]+)') { $matches[1] } else { "" }
        $hits = if ($actor -and $request) { @($zoneLines | Where-Object { $_ -like "*message-follow probe handled. actor=$actor, probe=$request,*" }).Count } else { 0 }
        Add-Verdict "ZW-B6" ([bool]$actor -and [bool]$request -and $hits -eq 1) "request exact-once handler hits=$hits"
    }

    if (Test-Scenario "ZW-G3") {
        $old = $Rid2
        Stop-Node "zone-node-2" "TERM"
        $opsFirst = Get-NextLogLine "ops"
        Start-Role "zone-node-replacement" $ServerBin @("--config", (Join-Path $ConfigDir "zone-node-2-replacement.properties")) | Out-Null
        if (Wait-LogWhileRunning "zone-node-replacement" "topology=ready" 1 $NodeProcesses["zone-node-replacement"] 900) {
            $new = Get-RoutingId "zone-node-2" $opsFirst
            $first = @((Get-Content -LiteralPath $ClientLog)).Count + 1
            $fresh = (Test-ZoneRoutingId $new) -and $new -ne $old -and (Invoke-Client "ZW-G3-fresh")
            $freshLines = @(Get-Content -LiteralPath $ClientLog | Select-Object -Skip ($first - 1))
            Add-Verdict "ZW-G3" ($fresh -and ($freshLines | Select-String -SimpleMatch "scenario ZW-G3-fresh owner=$new " -Quiet)) "replacement RID/fresh object placement failed"
        } else { Add-Verdict "ZW-G3" $false "replacement did not reach topology ready" }
    }

    if ($Scenario -eq "all") {
        Assert-Phase "zoneworld-relocation=completed" @("ZW-B2", "ZW-B3", "ZW-B5", "ZW-B6", "ZW-B7", "ZW-B8", "ZW-F2")
        Assert-Phase "zoneworld-border-sync=completed" @("ZW-B1", "ZW-B4")
        Assert-Phase "zoneworld-ops-observe=completed" @("ZW-C1", "ZW-C2", "ZW-C3", "ZW-C4")
        Assert-Phase "zoneworld-ops-announce=completed" @("ZW-D1", "ZW-D1-subscribers", "ZW-D1-spots", "ZW-D2")
        Assert-Phase "zoneworld-ops-maintenance=completed" @("ZW-E1", "ZW-E2", "ZW-E3", "ZW-E4", "ZW-E5", "ZW-E6")
        Assert-Phase "zoneworld=completed" @(
            "ZW-A1", "ZW-A2", "ZW-A3", "ZW-A4", "ZW-A5",
            "ZW-B1", "ZW-B2", "ZW-B3", "ZW-B4", "ZW-B5", "ZW-B6", "ZW-B7", "ZW-B8",
            "ZW-C1", "ZW-C2", "ZW-C3", "ZW-C4", "ZW-D1", "ZW-D1-subscribers", "ZW-D1-spots", "ZW-D2",
            "ZW-E1", "ZW-E2", "ZW-E3", "ZW-E4", "ZW-E5", "ZW-E5-arm", "ZW-E6",
            "ZW-F1", "ZW-F1-population", "ZW-F2", "ZW-F3", "ZW-F4", "ZW-F4-no-push",
            "ZW-G1", "ZW-G2-rid", "ZW-G2", "ZW-G3", "ZW-G4", "ZW-G5")
    }
    Write-Host "==> logs: $LogDir"
    if ($Status -ne 0) { throw "One or more ZoneWorld runner verdicts failed." }
    $RunSucceeded = $true
} finally {
    for ($index = $Processes.Count - 1; $index -ge 0; $index--) {
        try { Stop-ZlinkSampleProcessTree -Process $Processes[$index] } catch {
            [Console]::Error.WriteLine("Process cleanup failed: $($_.Exception.Message)")
        }
    }
    if ($RedisContainer) { Remove-ZlinkSampleRedis $RedisContainer }
    if (-not $RunSucceeded -or $env:ZLINK_SAMPLE_KEEP_RUN_DIR -eq "1") {
        Write-Host "runDir=$RunDir"
    } else {
        Remove-Item -Recurse -Force -LiteralPath $RunDir -ErrorAction SilentlyContinue
    }
}

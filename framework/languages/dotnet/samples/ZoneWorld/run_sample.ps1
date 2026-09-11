param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RunnerArguments
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ScriptDir = $PSScriptRoot
. (Join-Path $ScriptDir "../sample_runner.ps1")

function Resolve-ZoneWorldRunnerArguments {
    param([string[]]$Arguments)

    $Resolved = [ordered]@{
        Scenario = "all"
        ScenarioSet = $false
        BrowserSmoke = $false
        NoBrowserSmoke = $false
        G4Child = $false
        B8Child = $false
    }
    foreach ($argument in $Arguments) {
        if ($argument -eq "--browser-smoke") {
            $Resolved.BrowserSmoke = $true
            $Resolved.NoBrowserSmoke = $false
        } elseif ($argument -eq "--no-browser-smoke") {
            $Resolved.BrowserSmoke = $false
            $Resolved.NoBrowserSmoke = $true
        } elseif ($argument -eq "--g4-child") {
            $Resolved.G4Child = $true
        } elseif ($argument -eq "--b8-child") {
            $Resolved.B8Child = $true
        } elseif ($argument.StartsWith("--")) {
            throw "Unknown option: $argument"
        } elseif ($Resolved.ScenarioSet) {
            throw "Only one scenario selector may be supplied."
        } else {
            $Resolved.Scenario = $argument
            $Resolved.ScenarioSet = $true
        }
    }
    return [pscustomobject]$Resolved
}

$ResolvedArguments = Resolve-ZoneWorldRunnerArguments $RunnerArguments
$Scenario = $ResolvedArguments.Scenario
$BrowserSmoke = $ResolvedArguments.BrowserSmoke
$NoBrowserSmoke = $ResolvedArguments.NoBrowserSmoke
$G4Child = $ResolvedArguments.G4Child
$B8Child = $ResolvedArguments.B8Child

$RunDir = New-SampleRunDirectory "zoneworld-dotnet"
$LogDir = Join-Path $RunDir "logs"
$ConfigDir = Join-Path $RunDir "config"
New-Item -ItemType Directory -Force -Path $LogDir, $ConfigDir | Out-Null
$RedisContainer = $null
$RunSucceeded = $false
$NodeProcesses = @{}
$ClientRunNumber = 0
$Status = 0
$G4Proven = $false
$B8Proven = $false
$TraceStream = $env:ZLINK_SAMPLE_TRACE_STREAM -eq "1"
$SpotDiscoveryTrace = $env:ZLINK_SAMPLE_SPOT_DISCOVERY_TRACE -ne "0"

function Test-ZoneWorldScenario {
    param([Parameter(Mandatory = $true)][string]$Id)

    if ($Scenario -eq "all") { return $true }
    return $Id -in @($Scenario.Split(',', [System.StringSplitOptions]::RemoveEmptyEntries))
}

function Invoke-ZoneWorldChild {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    $powerShell = (Get-Process -Id $PID).Path
    $child = Start-SampleProcess -Name $Name -FilePath $powerShell -LogDirectory $LogDir `
        -Arguments (@("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $PSCommandPath) + $Arguments)
    try {
        Wait-SampleProcess -Process $child -Description "ZoneWorld $Name" -TimeoutSeconds 900
    }
    finally {
        $stdout = Join-Path $LogDir "$Name.out.log"
        $stderr = Join-Path $LogDir "$Name.err.log"
        if (Test-Path -LiteralPath $stdout) { Get-Content -LiteralPath $stdout | Write-Host }
        if (Test-Path -LiteralPath $stderr) { Get-Content -LiteralPath $stderr | Write-Host }
    }
}

function Get-ZoneWorldLogPath {
    param([Parameter(Mandatory = $true)][string]$Name)
    return Join-Path $LogDir "$Name.out.log"
}

function Get-ZoneWorldErrorLogPath {
    param([Parameter(Mandatory = $true)][string]$Name)
    return Join-Path $LogDir "$Name.err.log"
}

function Get-ZoneWorldNextLogLine {
    param([Parameter(Mandatory = $true)][string]$Name)

    $path = Get-ZoneWorldLogPath $Name
    if (-not (Test-Path -LiteralPath $path)) { return 1 }
    return @((Get-Content -LiteralPath $path)).Count + 1
}

function Get-ZoneWorldNextErrorLogLine {
    param([Parameter(Mandatory = $true)][string]$Name)

    $path = Get-ZoneWorldErrorLogPath $Name
    if (-not (Test-Path -LiteralPath $path)) { return 1 }
    return @((Get-Content -LiteralPath $path)).Count + 1
}

function Wait-ZoneWorldLog {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [int]$FirstLine = 1,
        [int]$Attempts = 200
    )

    $path = Get-ZoneWorldLogPath $Name
    for ($attempt = 0; $attempt -lt $Attempts; $attempt++) {
        if (Test-Path -LiteralPath $path) {
            $lines = @(Get-Content -LiteralPath $path | Select-Object -Skip ($FirstLine - 1))
            if ($lines | Select-String -SimpleMatch $Pattern -Quiet) { return }
        }
        Start-Sleep -Milliseconds 100
    }
    $tail = if (Test-Path -LiteralPath $path) {
        (Get-Content -LiteralPath $path -Tail 20) -join [Environment]::NewLine
    } else { "<missing log>" }
    throw "$Name never logged '$Pattern' after line $FirstLine.`n$tail"
}

function Wait-ZoneWorldOwnerLog {
    param(
        [Parameter(Mandatory = $true)][string]$Pattern,
        [int]$Attempts = 200
    )

    for ($attempt = 0; $attempt -lt $Attempts; $attempt++) {
        foreach ($name in @("zone-node-1", "zone-node-2")) {
            $path = Get-ZoneWorldLogPath $name
            if ((Test-Path -LiteralPath $path) -and
                (Select-String -LiteralPath $path -SimpleMatch $Pattern -Quiet)) { return }
        }
        Start-Sleep -Milliseconds 100
    }
    throw "No zone owner logged '$Pattern'."
}

function Get-ZoneWorldRoutingId {
    param(
        [Parameter(Mandatory = $true)][string]$NodeId,
        [int]$FirstLine = 1
    )

    $path = Get-ZoneWorldLogPath "ops"
    $pattern = "node status observed\. node=$([regex]::Escape($NodeId)), rid=([^ ,]+)"
    $matches = @(Get-Content -LiteralPath $path | Select-Object -Skip ($FirstLine - 1) |
        Select-String -Pattern $pattern)
    if ($matches.Count -eq 0) { throw "Ops did not report a routing id for $NodeId." }
    return $matches[-1].Matches[0].Groups[1].Value
}

function Test-ZoneWorldRoutingId {
    param([Parameter(Mandatory = $true)][string]$Value)
    return $Value -match '^zn-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$'
}

function Wait-ZoneWorldPeerAdmission {
    param(
        [Parameter(Mandatory = $true)][string]$FirstName,
        [Parameter(Mandatory = $true)][string]$FirstRid,
        [int]$FirstLine,
        [Parameter(Mandatory = $true)][string]$SecondName,
        [Parameter(Mandatory = $true)][string]$SecondRid,
        [int]$SecondLine,
        [int]$Attempts = 600
    )

    $firstPattern = "mesh_peer_admission_accepted local=$FirstRid peer=$SecondRid command=Admit"
    $secondPattern = "mesh_peer_admission_accepted local=$SecondRid peer=$FirstRid command=Admit"
    for ($attempt = 0; $attempt -lt $Attempts; $attempt++) {
        $firstPath = Get-ZoneWorldErrorLogPath $FirstName
        $secondPath = Get-ZoneWorldErrorLogPath $SecondName
        $firstAccepted = (Test-Path -LiteralPath $firstPath) -and
            (@(Get-Content -LiteralPath $firstPath | Select-Object -Skip ($FirstLine - 1)) |
                Select-String -SimpleMatch $firstPattern -Quiet)
        $secondAccepted = (Test-Path -LiteralPath $secondPath) -and
            (@(Get-Content -LiteralPath $secondPath | Select-Object -Skip ($SecondLine - 1)) |
                Select-String -SimpleMatch $secondPattern -Quiet)
        if ($firstAccepted -or $secondAccepted) { return }
        Start-Sleep -Milliseconds 100
    }
    throw "Neither side completed mesh admission for $FirstRid and $SecondRid."
}

function Wait-ZoneWorldEvidenceWhileRunning {
    param(
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)][string[]]$Names,
        [int]$Attempts = 600
    )

    for ($attempt = 0; $attempt -lt $Attempts; $attempt++) {
        if ((Get-ZoneWorldLogText $Names) -like "*$Pattern*") { return $true }
        if ($Process.HasExited) { return $false }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

function Start-ZoneWorldRole {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Project,
        [Parameter(Mandatory = $true)][string]$ConfigName
    )

    $existingOutput = Get-ZoneWorldLogPath $Name
    $existingError = Get-ZoneWorldErrorLogPath $Name
    if (Test-Path -LiteralPath $existingOutput) {
        Move-Item -LiteralPath $existingOutput -Destination (Join-Path $LogDir "$Name.$([Guid]::NewGuid().ToString('N')).out.log")
    }
    if (Test-Path -LiteralPath $existingError) {
        Move-Item -LiteralPath $existingError -Destination (Join-Path $LogDir "$Name.$([Guid]::NewGuid().ToString('N')).err.log")
    }
    $previousTrace = $env:ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY
    try {
        if ($SpotDiscoveryTrace) { $env:ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY = "1" }
        $process = Start-SampleDotnetAssembly -Name $Name -Project $Project -LogDirectory $LogDir `
            -Arguments @("--config", (Join-Path $ConfigDir "$ConfigName.json"))
    }
    finally {
        $env:ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY = $previousTrace
    }
    $NodeProcesses[$Name] = $process
    Write-Host "    started $Name (pid $($process.Id))"
    return $process
}

function Stop-ZoneWorldNode {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [switch]$Graceful
    )

    if (-not $NodeProcesses.ContainsKey($Name)) { return }
    $process = $NodeProcesses[$Name]
    Write-Host "    stopping $Name (pid $($process.Id))"
    Stop-SampleProcess -Process $process -Force:(-not $Graceful)
    $NodeProcesses.Remove($Name)
}

function Start-ZoneWorldNode {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [string]$ConfigName = ""
    )

    if (-not $ConfigName) { $ConfigName = $Name }
    if ($Name -eq "zone-node-2" -and $ConfigName -eq $Name) {
        $ConfigName = "zone-node-replacement"
    }
    $firstNodeLine = 1
    $firstNodeErrorLine = 1
    $firstOpsLine = Get-ZoneWorldNextLogLine "ops"
    $firstPeerErrorLine = Get-ZoneWorldNextErrorLogLine "zone-node-1"
    Start-ZoneWorldRole -Name $Name -Project $ZoneNodeProject -ConfigName $ConfigName | Out-Null
    Wait-ZoneWorldLog $Name "topology=ready" -FirstLine $firstNodeLine -Attempts 450
    Wait-ZoneWorldLog $Name "node status report submitted. node=$Name" -FirstLine $firstNodeLine -Attempts 450
    Wait-ZoneWorldLog "ops" "node connection observed. node=$Name, connected=True" -FirstLine $firstOpsLine -Attempts 450
    if ($Name -eq "zone-node-2") {
        $localRid = Get-ZoneWorldRoutingId "zone-node-2" -FirstLine $firstOpsLine
        $peerRid = Get-ZoneWorldRoutingId "zone-node-1"
        Wait-ZoneWorldPeerAdmission $Name $localRid $firstNodeErrorLine "zone-node-1" $peerRid $firstPeerErrorLine
    }
}

function Write-ZoneWorldConfig {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Role,
        [Parameter(Mandatory = $true)][hashtable]$Value
    )

    $body = @{ shared = $SharedSettings }
    $body[$Role] = $Value
    $body | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $ConfigDir "$Name.json") -Encoding UTF8
}

function Write-ZoneWorldClientConfig {
    param([Parameter(Mandatory = $true)][string]$Scenarios)

    $body = @{
        shared = $SharedSettings
        client = @{
            gatewayEndpoint = $GatewayEndpoint
            opsEndpoint = $OpsEndpoint
            scenarios = $Scenarios
            streamTrace = $TraceStream
            faultArmFile = Join-Path $RunDir "b8-block-command-44"
        }
    }
    $path = Join-Path $ConfigDir "client.json"
    $body | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $path -Encoding UTF8
    return $path
}

function Start-ZoneWorldClient {
    param([Parameter(Mandatory = $true)][string]$Scenarios)

    $script:ClientRunNumber++
    $name = "client-$($script:ClientRunNumber)"
    $config = Write-ZoneWorldClientConfig $Scenarios
    $process = Start-SampleDotnetAssembly -Name $name -Project $ClientProject -LogDirectory $LogDir `
        -Arguments @("--config", $config)
    return [pscustomobject]@{
        Name = $name
        Process = $process
        LogPath = Get-ZoneWorldLogPath $name
        ErrorPath = Join-Path $LogDir "$name.err.log"
    }
}

function Complete-ZoneWorldClient {
    param(
        [Parameter(Mandatory = $true)]$Run,
        [int]$TimeoutSeconds = 180
    )

    try {
        Wait-SampleProcess -Process $Run.Process -Description "ZoneWorld client" -TimeoutSeconds $TimeoutSeconds
    }
    finally {
        if (Test-Path -LiteralPath $Run.LogPath) {
            Get-Content -LiteralPath $Run.LogPath | Tee-Object -FilePath $ClientLog -Append | Write-Host
        }
        if (Test-Path -LiteralPath $Run.ErrorPath) {
            Get-Content -LiteralPath $Run.ErrorPath | Add-Content -LiteralPath $ClientErrorLog
        }
    }
}

function Invoke-ZoneWorldClient {
    param([Parameter(Mandatory = $true)][string]$Scenarios)
    $run = Start-ZoneWorldClient $Scenarios
    Complete-ZoneWorldClient $run
}

function Invoke-ZoneWorldClientWithStop {
    param(
        [Parameter(Mandatory = $true)][string]$Id,
        [Parameter(Mandatory = $true)][string]$Node,
        [switch]$Graceful
    )

    if (-not (Test-ZoneWorldScenario $Id)) { return }
    $run = Start-ZoneWorldClient $Id
    Wait-ZoneWorldLog $run.Name "scenario $Id armed" -Attempts 600
    if ($Node -eq "auto") {
        $armed = @(Get-Content -LiteralPath $run.LogPath |
            Select-String -Pattern "scenario $([regex]::Escape($Id)) armed node=([^ ]+)")[-1]
        if ($null -eq $armed) { throw "$Id did not identify the node to stop." }
        $Node = $armed.Matches[0].Groups[1].Value
    }
    Stop-ZoneWorldNode $Node -Graceful:$Graceful
    Complete-ZoneWorldClient $run
    Start-ZoneWorldNode $Node
}

function Add-ZoneWorldVerdict {
    param(
        [Parameter(Mandatory = $true)][string]$Id,
        [Parameter(Mandatory = $true)][bool]$Passed,
        [string]$Failure = ""
    )

    $line = if ($Passed) { "scenario $Id passed" } else { "scenario $Id failed" }
    Add-Content -LiteralPath $RunnerLog -Value $line
    if ($Passed) { Write-Host $line }
    else {
        $script:Status = 1
        Write-Warning "scenario $Id FAILED: $Failure"
    }
}

function Test-ZoneWorldVerdictSelected {
    param([Parameter(Mandatory = $true)][string]$Id)
    if ($Scenario -eq "all") { return $true }
    $parts = $Id.Split('-')
    $base = if ($parts.Count -ge 2) { "$($parts[0])-$($parts[1])" } else { $Id }
    return $base -in @($Scenario.Split(',', [System.StringSplitOptions]::RemoveEmptyEntries))
}

function Get-ZoneWorldLogText {
    param([Parameter(Mandatory = $true)][string[]]$Names)
    return ($Names | ForEach-Object {
        $name = $_
        Get-ChildItem -LiteralPath $LogDir -File -Filter "$name.out.log" -ErrorAction SilentlyContinue |
            ForEach-Object { Get-Content -Raw -LiteralPath $_.FullName }
        Get-ChildItem -LiteralPath $LogDir -File -Filter "$name.*.out.log" -ErrorAction SilentlyContinue |
            ForEach-Object { Get-Content -Raw -LiteralPath $_.FullName }
    }) -join [Environment]::NewLine
}

function Test-ZoneWorldEveryLog {
    param(
        [Parameter(Mandatory = $true)][string[]]$Names,
        [Parameter(Mandatory = $true)][string]$Pattern
    )
    foreach ($name in $Names) {
        if ((Get-ZoneWorldLogText @($name)) -notlike "*$Pattern*") { return $false }
    }
    return $true
}

function Assert-ZoneWorldPhase {
    param(
        [Parameter(Mandatory = $true)][string]$Marker,
        [Parameter(Mandatory = $true)][string[]]$Ids
    )

    $verdictText = (Get-Content -Raw -LiteralPath $ClientLog) + [Environment]::NewLine +
        (Get-Content -Raw -LiteralPath $RunnerLog)
    foreach ($id in $Ids) {
        if ($verdictText -notmatch "(?m)^scenario $([regex]::Escape($id)) passed$") {
            throw "$Marker withheld: $id did not pass."
        }
    }
    if ($script:Status -ne 0) { throw "$Marker withheld: a selected verdict failed." }
    Write-Host $Marker
}

try {
    if (-not $G4Child -and (Test-ZoneWorldScenario "ZW-G4")) {
        Invoke-ZoneWorldChild "g4-child" @("--g4-child", "--no-browser-smoke", "ZW-G4")
        $G4Proven = $true
        if ($Scenario -eq "ZW-G4") { $RunSucceeded = $true; return }
    }
    if (-not $B8Child -and (Test-ZoneWorldScenario "ZW-B8")) {
        Invoke-ZoneWorldChild "b8-child" @("--b8-child", "--no-browser-smoke", "ZW-B8")
        $B8Proven = $true
        if ($Scenario -eq "ZW-B8") { $RunSucceeded = $true; return }
    }

    $OpsProject = Join-Path $ScriptDir "Server/Ops/ZoneWorld.Server.Ops.csproj"
    $ZoneNodeProject = Join-Path $ScriptDir "Server/ZoneNode/ZoneWorld.Server.ZoneNode.csproj"
    $GatewayProject = Join-Path $ScriptDir "Server/Gateway/ZoneWorld.Server.Gateway.csproj"
    $ClientProject = Join-Path $ScriptDir "Client/ZoneWorld.Client.csproj"
    foreach ($project in @($OpsProject, $ZoneNodeProject, $GatewayProject, $ClientProject)) {
        Invoke-SampleDotnetBuild $project
    }

    $ports = @(New-SamplePorts -Count 9)
    $GatewayEndpoint = "ws://127.0.0.1:$($ports[6])"
    $OpsEndpoint = "ws://127.0.0.1:$($ports[4])"
    $BrowserPreviewPort = $ports[8]
    $redis = Start-SampleRedisContainer "zlink-zoneworld-dotnet-redis"
    $RedisContainer = $redis.ContainerId
    Wait-SampleTcpEndpoint "redis" "tcp://$($redis.Endpoint)"
    $SharedSettings = @{
        redisEndpoint = $redis.Endpoint
        redisKeyPrefix = "zoneworld-$([Guid]::NewGuid().ToString('N')):"
        logDirectory = $LogDir
    }

    for ($index = 1; $index -le 3; $index++) {
        $useProxy = $B8Child -and $index -lt 3
        $meshHost = if ($useProxy) { "127.0.0.2" } else { "127.0.0.1" }
        Write-ZoneWorldConfig "zone-node-$index" "zoneNode" @{
            nodeId = "zone-node-$index"
            meshEndpoint = "tcp://${meshHost}:$($ports[$index - 1])"
            meshAdvertiseHost = if ($useProxy) { "127.0.0.1" } else { $null }
            faultTickZone = if ($index -in @(1, 2)) { "zone-nw" } else { $null }
            disableBots = $false
            subscriberOnly = $index -eq 3
        }
    }
    Write-ZoneWorldConfig "zone-node-crash-replacement" "zoneNode" @{
        nodeId = "zone-node-2"; meshEndpoint = "tcp://127.0.0.1:$($ports[3])"
        faultTickZone = $null; disableBots = $true; subscriberOnly = $false; allowEmptyZoneSet = $true
    }
    Write-ZoneWorldConfig "zone-node-replacement" "zoneNode" @{
        nodeId = "zone-node-2"; meshEndpoint = "tcp://127.0.0.1:$($ports[3])"
        faultTickZone = $null; disableBots = $false; subscriberOnly = $false
    }
    Write-ZoneWorldConfig "ops" "ops" @{
        streamEndpoint = $OpsEndpoint; meshEndpoint = "tcp://127.0.0.1:$($ports[5])"
    }
    $gatewayMeshHost = if ($B8Child) { "127.0.0.2" } else { "127.0.0.1" }
    Write-ZoneWorldConfig "gateway" "gateway" @{
        streamEndpoint = $GatewayEndpoint
        meshEndpoint = "tcp://${gatewayMeshHost}:$($ports[7])"
        meshAdvertiseHost = if ($B8Child) { "127.0.0.1" } else { $null }
    }

    $ClientLog = Join-Path $LogDir "client.log"
    $ClientErrorLog = Join-Path $LogDir "client.err.log"
    $RunnerLog = Join-Path $LogDir "runner.log"
    New-Item -ItemType File -Force -Path $ClientLog, $ClientErrorLog, $RunnerLog | Out-Null

    Write-Host "==> ops"
    Start-ZoneWorldRole "ops" $OpsProject "ops" | Out-Null
    Wait-ZoneWorldLog "ops" "Application started."

    if ($B8Child) {
        $python = Get-Command python -ErrorAction SilentlyContinue
        if ($null -eq $python) { $python = Get-Command py -ErrorAction SilentlyContinue }
        if ($null -eq $python) { throw "Python is required for the ZW-B8 fault proxy." }
        foreach ($proxy in @(
            @{ Name = "session-route-proxy-zone-node-1"; Port = $ports[0] },
            @{ Name = "session-route-proxy-zone-node-2"; Port = $ports[1] },
            @{ Name = "session-route-proxy-gateway"; Port = $ports[7] }
        )) {
            $arguments = @()
            if ($python.Name -eq "py.exe") { $arguments += "-3" }
            $arguments += @(
                (Join-Path $ScriptDir "Support/session_route_block_proxy.py"),
                "--listen-host", "127.0.0.1", "--listen-port", "$($proxy.Port)",
                "--target-host", "127.0.0.2", "--target-port", "$($proxy.Port)",
                "--arm-file", (Join-Path $RunDir "b8-block-command-44")
            )
            Start-SampleProcess $proxy.Name $python.Source $LogDir -Arguments $arguments | Out-Null
            Wait-ZoneWorldLog $proxy.Name "proxy-ready"
        }
    }

    Write-Host "==> zone nodes"
    if ((Test-ZoneWorldScenario "ZW-G2") -and -not $G4Child) {
        Start-ZoneWorldRole "zone-node-2" $ZoneNodeProject "zone-node-2" | Out-Null
        Start-ZoneWorldRole "zone-node-1" $ZoneNodeProject "zone-node-1" | Out-Null
    }
    else {
        Start-ZoneWorldRole "zone-node-1" $ZoneNodeProject "zone-node-1" | Out-Null
        Start-ZoneWorldRole "zone-node-2" $ZoneNodeProject "zone-node-2" | Out-Null
    }
    Wait-ZoneWorldLog "zone-node-1" "topology=ready"
    Wait-ZoneWorldLog "zone-node-2" "topology=ready"
    Wait-ZoneWorldLog "ops" "node status observed. node=zone-node-1, rid=zn-"
    Wait-ZoneWorldLog "ops" "node status observed. node=zone-node-2, rid=zn-"
    $node1Rid = Get-ZoneWorldRoutingId "zone-node-1"
    $node2Rid = Get-ZoneWorldRoutingId "zone-node-2"
    Wait-ZoneWorldPeerAdmission "zone-node-1" $node1Rid 1 "zone-node-2" $node2Rid 1

    if ($G4Proven) { Add-ZoneWorldVerdict "ZW-G4" $true }
    if ($B8Proven) { Add-ZoneWorldVerdict "ZW-B8" $true }
    if ((Test-ZoneWorldScenario "ZW-G1") -and -not $G4Child) {
        Add-ZoneWorldVerdict "ZW-G1" ((Test-ZoneWorldRoutingId $node1Rid) -and
            (Test-ZoneWorldRoutingId $node2Rid) -and $node1Rid -ne $node2Rid) "RIDs were not distinct zn-UUIDv4 values."
    }
    if ((Test-ZoneWorldScenario "ZW-G2") -and -not $G4Child) {
        Add-ZoneWorldVerdict "ZW-G2-rid" (Test-ZoneWorldRoutingId $node2Rid) "Reverse-started node did not publish a canonical RID."
    }
    if (Test-ZoneWorldScenario "ZW-G5") {
        $fixedRidHits = @(Get-ChildItem -Path (Join-Path $ScriptDir "Server/ZoneNode"),
            (Join-Path $ScriptDir "Server/Configuration"), $ConfigDir -Recurse -File -Include *.cs,*.json |
            Where-Object { $_.FullName -notmatch '[\\/](bin|obj)[\\/]' } |
            Select-String -Pattern 'SetRoutingId\(|(^|[^A-Za-z0-9])zn[12]([^A-Za-z0-9]|$)')
        Add-ZoneWorldVerdict "ZW-G5" ($fixedRidHits.Count -eq 0) "A fixed routing id was found."
    }

    Write-Host "==> zone-node-3"
    Start-ZoneWorldRole "zone-node-3" $ZoneNodeProject "zone-node-3" | Out-Null
    Wait-ZoneWorldLog "zone-node-3" "topology=ready"

    Write-Host "==> gateway"
    Start-ZoneWorldRole "gateway" $GatewayProject "gateway" | Out-Null
    Wait-ZoneWorldLog "gateway" "Application started."
    Wait-ZoneWorldOwnerLog "border subscription ready. zone=zone-nw, from=zone-ne"
    Wait-ZoneWorldOwnerLog "border subscription ready. zone=zone-sw, from=zone-se"
    Wait-ZoneWorldOwnerLog "border subscription ready. zone=zone-ne, from=zone-nw"
    Wait-ZoneWorldOwnerLog "border subscription ready. zone=zone-se, from=zone-sw"

    if ($B8Child) {
        $run = Start-ZoneWorldClient "ZW-B8"
        Wait-ZoneWorldLog $run.Name "scenario ZW-B8 armed actor=" -Attempts 600
        $armed = @(Get-Content -LiteralPath $run.LogPath |
            Select-String -Pattern 'scenario ZW-B8 armed actor=([^ ]+) target=([^ ]+)')[-1]
        if ($null -eq $armed) { throw "ZW-B8 did not identify its actor and target." }
        $actor = $armed.Matches[0].Groups[1].Value
        $target = $armed.Matches[0].Groups[2].Value
        New-Item -ItemType File -Force -Path (Join-Path $RunDir "b8-block-command-44") | Out-Null
        $proxyPatternSeen = Wait-ZoneWorldEvidenceWhileRunning 'blocked-command-44' $run.Process @(
            "session-route-proxy-zone-node-1", "session-route-proxy-zone-node-2", "session-route-proxy-gateway")
        if (-not $proxyPatternSeen) { throw "ZW-B8 fault proxy did not intercept command 44." }
        $commitPatternSeen = Wait-ZoneWorldEvidenceWhileRunning `
            "zone spot: player entered. zone=$target, player=$actor, bot=False, initial=False" `
            $run.Process @("zone-node-1", "zone-node-2")
        Remove-Item -Force -LiteralPath (Join-Path $RunDir "b8-block-command-44") -ErrorAction SilentlyContinue
        if (-not $commitPatternSeen) { throw "ZW-B8 target relocation commit was not observed." }
        Complete-ZoneWorldClient $run
        Add-ZoneWorldVerdict "ZW-B8" $true
        $RunSucceeded = $true
        return
    }

    if ($G4Child) {
        $oldRid = $node2Rid
        $run = Start-ZoneWorldClient "ZW-G4"
        Wait-ZoneWorldLog $run.Name "scenario ZW-G4 armed node=zone-node-2" -Attempts 600
        Wait-ZoneWorldLog "zone-node-2" "crash-boundary join pending" -Attempts 600
        Stop-ZoneWorldNode "zone-node-2"
        Complete-ZoneWorldClient $run
        $firstReplacementOpsLine = Get-ZoneWorldNextLogLine "ops"
        Start-ZoneWorldNode "zone-node-2" "zone-node-crash-replacement"
        $crashRid = Get-ZoneWorldRoutingId "zone-node-2" -FirstLine $firstReplacementOpsLine
        if (-not (Test-ZoneWorldRoutingId $crashRid) -or $crashRid -eq $oldRid) {
            throw "ZW-G4 crash replacement did not publish a new canonical RID."
        }
        Invoke-ZoneWorldClient "ZW-G4-fresh"
        if (-not (Select-String -LiteralPath $ClientLog -SimpleMatch "scenario ZW-G4-fresh owner=$crashRid " -Quiet)) {
            throw "ZW-G4 did not place a fresh Actor on the replacement RID."
        }
        Add-ZoneWorldVerdict "ZW-G4" $true
        $RunSucceeded = $true
        return
    }

    if ($Scenario -eq "all" -or (Test-ZoneWorldScenario "ZW-G2")) {
        Invoke-ZoneWorldClient "ZW-G2"
    }

    if ($BrowserSmoke -and -not $NoBrowserSmoke) {
        Write-Host "==> shared browser client"
        $browserRoot = (Resolve-Path (Join-Path $ScriptDir "../../../shared_sample/zoneworld/client")).Path
        $browserDist = Join-Path $RunDir "browser-dist"
        $browserMarker = Join-Path $RunDir "browser-lifecycle-armed"
        $browserConfig = Join-Path $RunDir "playwright.live.config.mjs"
        Push-Location $browserRoot
        try {
            & npm exec vite build -- --outDir $browserDist
            if ($LASTEXITCODE -ne 0) { throw "ZoneWorld browser build failed with exit code $LASTEXITCODE." }
        }
        finally { Pop-Location }
        @{ gateway = $GatewayEndpoint; ops = $OpsEndpoint } | ConvertTo-Json |
            Set-Content -LiteralPath (Join-Path $browserDist "config.json") -Encoding UTF8
        $playwright = @{
            testDir = (Join-Path $browserRoot "tests/live")
            timeout = 45000
            workers = 1
            use = @{ baseURL = "http://127.0.0.1:$BrowserPreviewPort"; headless = $true }
            metadata = @{ lifecycleMarker = $browserMarker; lifecycleNodeId = "zone-node-2" }
        }
        "export default $($playwright | ConvertTo-Json -Depth 6);" |
            Set-Content -LiteralPath $browserConfig -Encoding UTF8
        $npm = (Get-Command npm.cmd -ErrorAction Stop).Source
        $preview = Start-SampleProcess "browser-preview" $npm $LogDir -WorkingDirectory $browserRoot `
            -Arguments @("exec", "vite", "preview", "--", "--host", "127.0.0.1", "--port", "$BrowserPreviewPort", "--outDir", $browserDist)
        Wait-SampleTcpEndpoint "browser preview" "tcp://127.0.0.1:$BrowserPreviewPort" -Attempts 200
        $browser = Start-SampleProcess "browser" $npm $LogDir -WorkingDirectory $browserRoot `
            -Arguments @("exec", "playwright", "test", "--", "--config", $browserConfig)
        for ($attempt = 0; $attempt -lt 450 -and -not (Test-Path -LiteralPath $browserMarker); $attempt++) {
            if ($browser.HasExited) { break }
            Start-Sleep -Milliseconds 100
        }
        if (-not (Test-Path -LiteralPath $browserMarker)) {
            throw "ZoneWorld browser client did not arm its node lifecycle check."
        }
        Stop-ZoneWorldNode "zone-node-2"
        Wait-SampleProcess -Process $browser -Description "ZoneWorld browser" -TimeoutSeconds 90
        Start-ZoneWorldNode "zone-node-2"
        Stop-SampleProcess -Process $preview -Force
    }

    $excluded = @("ZW-D2", "ZW-F2", "ZW-C2", "ZW-C3", "ZW-B4", "ZW-E5", "ZW-E5-arm",
        "ZW-G1", "ZW-G2", "ZW-G3", "ZW-G4", "ZW-G5")
    $clientScenarios = @($Scenario.Split(',', [System.StringSplitOptions]::RemoveEmptyEntries) |
        Where-Object { $_ -notin $excluded })
    if ($Scenario -eq "all") { Invoke-ZoneWorldClient "all" }
    elseif ($clientScenarios.Count -gt 0) { Invoke-ZoneWorldClient ($clientScenarios -join ',') }

    $zoneLogs = Get-ZoneWorldLogText @("zone-node-1", "zone-node-2", "zone-node-3")
    if (Test-ZoneWorldVerdictSelected "ZW-B5") {
        $line = @(Select-String -LiteralPath $ClientLog -Pattern 'message-follow-one-way completed actor=([^ ]+) probe=([^ ]+)')[-1]
        $passed = $null -ne $line
        if ($passed) {
            $actor = $line.Matches[0].Groups[1].Value; $probe = $line.Matches[0].Groups[2].Value
            $payload = [BitConverter]::ToString([Text.Encoding]::UTF8.GetBytes("one-way-payload")).Replace('-', '')
            $handlerCount = @($zoneLogs -split "`r?`n" | Where-Object {
                $_ -like "*message-follow probe one-way handled.*actor=$actor,*probe=$probe,*payload=$payload*"
            }).Count
            $relayCount = @($zoneLogs -split "`r?`n" | Where-Object {
                $_ -like "*message_follow_relay*actor=$actor*"
            }).Count
            $passed = $handlerCount -eq 1 -and $relayCount -eq 1
        }
        Add-ZoneWorldVerdict "ZW-B5" $passed "One-way Follow evidence was incomplete."
    }
    if (Test-ZoneWorldVerdictSelected "ZW-B6") {
        $line = @(Select-String -LiteralPath $ClientLog -Pattern 'message-follow-request completed actor=([^ ]+) request=([^ ]+)')[-1]
        $passed = $null -ne $line
        if ($passed) {
            $actor = $line.Matches[0].Groups[1].Value; $request = $line.Matches[0].Groups[2].Value
            $payload = [BitConverter]::ToString([Text.Encoding]::UTF8.GetBytes("request-payload")).Replace('-', '')
            $handlerCount = @($zoneLogs -split "`r?`n" | Where-Object {
                $_ -like "*message-follow probe handled.*actor=$actor,*probe=$request,*payload=$payload*"
            }).Count
            $relayCount = @($zoneLogs -split "`r?`n" | Where-Object {
                $_ -like "*message_follow_relay*actor=$actor*"
            }).Count
            $passed = $handlerCount -eq 1 -and $relayCount -eq 1
        }
        Add-ZoneWorldVerdict "ZW-B6" $passed "Request Follow evidence was incomplete."
    }

    Invoke-ZoneWorldClientWithStop "ZW-B4" "auto"
    Invoke-ZoneWorldClientWithStop "ZW-C2" "zone-node-2" -Graceful
    Invoke-ZoneWorldClientWithStop "ZW-C3" "zone-node-2"
    if (Test-ZoneWorldScenario "ZW-E5") {
        Invoke-ZoneWorldClient "ZW-E5-arm"
        Stop-ZoneWorldNode "zone-node-2"
        Start-ZoneWorldNode "zone-node-2"
        Invoke-ZoneWorldClient "ZW-E5"
    }

    if (Test-ZoneWorldVerdictSelected "ZW-D1-subscribers") {
        Add-ZoneWorldVerdict "ZW-D1-subscribers" `
            (Test-ZoneWorldEveryLog @("zone-node-1", "zone-node-2") "fanout subscriber received announcement") `
            "A node fanout subscriber did not receive the announcement."
    }
    if (Test-ZoneWorldVerdictSelected "ZW-D1-spots") {
        Add-ZoneWorldVerdict "ZW-D1-spots" `
            (Test-ZoneWorldEveryLog @("zone-node-1", "zone-node-2") "zone spot: announcement delivered") `
            "A zone spot did not receive the announcement."
    }
    if (Test-ZoneWorldVerdictSelected "ZW-D2") {
        Add-ZoneWorldVerdict "ZW-D2" `
            ((Get-ZoneWorldLogText @("zone-node-3")) -like '*fanout subscriber received announcement*') `
            "Zone-node-3 did not receive the announcement."
    }
    if (Test-ZoneWorldVerdictSelected "ZW-F4-no-push") {
        Add-ZoneWorldVerdict "ZW-F4-no-push" `
            ($zoneLogs -notlike "*No current session binding exists for actor 'bot-*") `
            "A push was attempted to a bot."
    }
    if (Test-ZoneWorldVerdictSelected "ZW-F1-population") {
        $expectedBots = @(
            'bot=bot-nw-x, zone=zone-nw, start=(10,15), dir=(1,0)', 'bot=bot-nw-y, zone=zone-nw, start=(15,10), dir=(0,1)',
            'bot=bot-ne-x, zone=zone-ne, start=(90,15), dir=(-1,0)', 'bot=bot-ne-y, zone=zone-ne, start=(85,10), dir=(0,1)',
            'bot=bot-sw-x, zone=zone-sw, start=(10,85), dir=(1,0)', 'bot=bot-sw-y, zone=zone-sw, start=(15,90), dir=(0,-1)',
            'bot=bot-se-x, zone=zone-se, start=(90,85), dir=(-1,0)', 'bot=bot-se-y, zone=zone-se, start=(85,90), dir=(0,-1)'
        )
        $allBots = @($zoneLogs -split "`r?`n" | Select-String -Pattern 'bot spawned\. bot=([a-z0-9-]+)' |
            ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique)
        $fixedRoster = @($expectedBots | Where-Object { $zoneLogs -notlike "*$_*" }).Count -eq 0
        Add-ZoneWorldVerdict "ZW-F1-population" ($allBots.Count -eq 8 -and $fixedRoster) "The fixed eight-bot roster was not observed."
    }
    if (Test-ZoneWorldVerdictSelected "ZW-F2") {
        $correlated = $false
        for ($attempt = 0; $attempt -lt 600 -and -not $correlated; $attempt++) {
            $node1 = Get-ZoneWorldLogText @("zone-node-1")
            $node2 = Get-ZoneWorldLogText @("zone-node-2")
            $actors = @($node1 -split "`r?`n" | Select-String -Pattern 'player=(bot-[^,]+), bot=True, initial=False' |
                ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique)
            $correlated = @($actors | Where-Object { $node2 -like "*player=$_, bot=True, initial=False*" }).Count -gt 0
            if (-not $correlated) { Start-Sleep -Milliseconds 100 }
        }
        Add-ZoneWorldVerdict "ZW-F2" $correlated "No correlated cross-node bot handoff was observed."
    }

    if (Test-ZoneWorldScenario "ZW-G3") {
        $oldRid = $node2Rid
        Stop-ZoneWorldNode "zone-node-2" -Graceful
        $firstOpsLine = Get-ZoneWorldNextLogLine "ops"
        Start-ZoneWorldRole "zone-node-replacement" $ZoneNodeProject "zone-node-replacement" | Out-Null
        Wait-ZoneWorldLog "zone-node-replacement" "topology=ready" -Attempts 600
        Wait-ZoneWorldLog "ops" "node status observed. node=zone-node-2, rid=zn-" -FirstLine $firstOpsLine -Attempts 600
        $replacementRid = Get-ZoneWorldRoutingId "zone-node-2" -FirstLine $firstOpsLine
        $passed = (Test-ZoneWorldRoutingId $replacementRid) -and $replacementRid -ne $oldRid
        if ($passed) {
            try { Invoke-ZoneWorldClient "ZW-A1" } catch { $passed = $false }
        }
        Add-ZoneWorldVerdict "ZW-G3" $passed "Normal replacement did not publish a new RID and accept a fresh object."
    }

    if ($Scenario -eq "all") {
        Assert-ZoneWorldPhase "zoneworld-relocation=completed" @("ZW-B2", "ZW-B3", "ZW-B5", "ZW-B6", "ZW-B7", "ZW-B8", "ZW-F2")
        Assert-ZoneWorldPhase "zoneworld-border-sync=completed" @("ZW-B1", "ZW-B4")
        Assert-ZoneWorldPhase "zoneworld-ops-observe=completed" @("ZW-C1", "ZW-C2", "ZW-C3", "ZW-C4")
        Assert-ZoneWorldPhase "zoneworld-ops-announce=completed" @("ZW-D1", "ZW-D1-subscribers", "ZW-D1-spots", "ZW-D2")
        Assert-ZoneWorldPhase "zoneworld-ops-maintenance=completed" @("ZW-E1", "ZW-E2", "ZW-E3", "ZW-E4", "ZW-E5", "ZW-E6")
        Assert-ZoneWorldPhase "zoneworld=completed" @(
            "ZW-A1", "ZW-A2", "ZW-A3", "ZW-A4", "ZW-A5",
            "ZW-B1", "ZW-B2", "ZW-B3", "ZW-B4", "ZW-B5", "ZW-B6", "ZW-B7", "ZW-B8",
            "ZW-C1", "ZW-C2", "ZW-C3", "ZW-C4",
            "ZW-D1", "ZW-D1-subscribers", "ZW-D1-spots", "ZW-D2",
            "ZW-E1", "ZW-E2", "ZW-E3", "ZW-E4", "ZW-E5", "ZW-E5-arm", "ZW-E6",
            "ZW-F1", "ZW-F1-population", "ZW-F2", "ZW-F3", "ZW-F4", "ZW-F4-no-push",
            "ZW-G1", "ZW-G2-rid", "ZW-G2", "ZW-G3", "ZW-G4", "ZW-G5")
    }
    if ($Status -ne 0) { throw "One or more ZoneWorld runner verdicts failed." }
    $RunSucceeded = $true
}
finally {
    $cleanupFailures = @()
    try { Remove-SampleConfigurationFiles -RunDirectory $RunDir }
    catch { $cleanupFailures += "Configuration cleanup failed: $($_.Exception.Message)" }
    try { Stop-SampleProcesses }
    catch { $cleanupFailures += $_.Exception.Message }
    if ($RedisContainer) {
        try { Remove-SampleRedisContainer $RedisContainer }
        catch { $cleanupFailures += "Redis cleanup failed: $($_.Exception.Message)" }
    }
    if (-not $RunSucceeded -or $cleanupFailures.Count -gt 0 -or $env:ZONEWORLD_KEEP_RUN_DIR -eq "1") {
        Write-Host "runDir=$RunDir"
    }
    else {
        Remove-Item -Recurse -Force -LiteralPath $RunDir -ErrorAction SilentlyContinue
    }
    if ($cleanupFailures.Count -gt 0) {
        throw ($cleanupFailures -join [Environment]::NewLine)
    }
}

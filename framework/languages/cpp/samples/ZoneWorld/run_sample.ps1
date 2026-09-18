[CmdletBinding()]
param(
    [switch]$B8Child,
    [switch]$G4Child
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

. "$PSScriptRoot/../redis-common.ps1"

$CppRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$BuildDir = if ($env:ZLINK_CPP_BUILD_DIR) { $env:ZLINK_CPP_BUILD_DIR } else { Join-Path $CppRoot "build" }
$BuildConfiguration = if ($env:ZLINK_CPP_BUILD_CONFIGURATION) { $env:ZLINK_CPP_BUILD_CONFIGURATION } else { "Release" }
$RunDir = Join-Path ([System.IO.Path]::GetTempPath()) "zoneworld-cpp-$PID-$([Guid]::NewGuid().ToString('N'))"
$LogDir = Join-Path $RunDir "logs"
$ConfigDir = Join-Path $RunDir "config"
New-Item -ItemType Directory -Force -Path $LogDir, $ConfigDir | Out-Null

function Read-ScenarioManifest([string]$Path) {
    $assignment = Get-Content -LiteralPath $Path | Where-Object { $_ -match '^ZONEWORLD_SCENARIOS=' } | Select-Object -First 1
    if (-not $assignment) { throw "ZONEWORLD_SCENARIOS was not found in $Path" }
    $value = $assignment.Split('=', 2)[1].Trim().Trim([char[]]@('"', "'"))
    return @($value.Split(' ', [System.StringSplitOptions]::RemoveEmptyEntries))
}

$ExpectedIds = @(Read-ScenarioManifest (Join-Path $PSScriptRoot "sample-manifest.env"))
if ($ExpectedIds.Count -eq 0) { throw "ZoneWorld scenario manifest is empty." }

function Find-Binary([string]$Name) {
    foreach ($candidate in @(
        (Join-Path $BuildDir $Name), (Join-Path $BuildDir "$Name.exe"),
        (Join-Path $BuildDir "$BuildConfiguration/$Name"), (Join-Path $BuildDir "$BuildConfiguration/$Name.exe"),
        (Join-Path $BuildDir "linux-ninja-debug/$Name"), (Join-Path $BuildDir "linux-ninja-debug/$Name.exe")
    )) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw "Missing executable: $Name"
}

function Wait-Port([string]$Name, [string]$Endpoint, [int]$TimeoutSeconds = 30) {
    $value = $Endpoint -replace '^tcp://', '' -replace '^http://', ''
    $separator = $value.LastIndexOf(':')
    $hostName = $value.Substring(0, $separator)
    $port = [int]$value.Substring($separator + 1)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $client = New-Object System.Net.Sockets.TcpClient
        try {
            $connect = $client.BeginConnect($hostName, $port, $null, $null)
            if ($connect.AsyncWaitHandle.WaitOne(200)) {
                $client.EndConnect($connect)
                return
            }
        } catch {
        } finally {
            $client.Dispose()
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for $Name at $Endpoint"
}

$Processes = New-Object System.Collections.Generic.List[System.Diagnostics.Process]
$RoleProcesses = @{}
$RoleStdout = @{}
$RoleStartCount = @{}

function Start-Role([string]$Name, [string]$Binary, [string[]]$Arguments) {
    $generation = if ($RoleStartCount.ContainsKey($Name)) { [int]$RoleStartCount[$Name] + 1 } else { 1 }
    $RoleStartCount[$Name] = $generation
    $stdout = Join-Path $LogDir "$Name-$generation.stdout.log"
    $stderr = Join-Path $LogDir "$Name-$generation.stderr.log"
    $process = Start-Process -FilePath $Binary -ArgumentList $Arguments -NoNewWindow -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    [void]$process.Handle
    [void]$Processes.Add($process)
    $RoleProcesses[$Name] = $process
    $RoleStdout[$Name] = $stdout
    return $process
}

function Remove-TrackedProcess([System.Diagnostics.Process]$Process) {
    [void]$Processes.Remove($Process)
}

function Stop-Role([string]$Name) {
    if (-not $RoleProcesses.ContainsKey($Name)) { return }
    $process = [System.Diagnostics.Process]$RoleProcesses[$Name]
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        [void]$process.WaitForExit(5000)
    }
    Remove-TrackedProcess $process
    [void]$RoleProcesses.Remove($Name)
}

function Wait-RoleExit([string]$Name, [int]$TimeoutMilliseconds) {
    if (-not $RoleProcesses.ContainsKey($Name)) { throw "Role is not running: $Name" }
    $process = [System.Diagnostics.Process]$RoleProcesses[$Name]
    if (-not $process.WaitForExit($TimeoutMilliseconds)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        [void]$process.WaitForExit(5000)
        throw "$Name timed out after $([int]($TimeoutMilliseconds / 1000)) seconds."
    }
    $exitCode = $process.ExitCode
    Remove-TrackedProcess $process
    [void]$RoleProcesses.Remove($Name)
    return $exitCode
}

function Get-Text([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return "" }
    try { return [string](Get-Content -LiteralPath $Path -Raw -ErrorAction Stop) } catch { return "" }
}

function Wait-RoleLog([string]$Name, [string]$Pattern, [int]$TimeoutSeconds = 60) {
    if (-not $RoleStdout.ContainsKey($Name)) { throw "Role log is not registered: $Name" }
    $path = [string]$RoleStdout[$Name]
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $text = [string](Get-Text $path)
        if ($text.Contains($Pattern)) { return $true }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

function Get-LogFiles([string[]]$Patterns) {
    $files = New-Object System.Collections.Generic.List[string]
    foreach ($pattern in $Patterns) {
        Get-ChildItem -LiteralPath $LogDir -Filter $pattern -File -ErrorAction SilentlyContinue | ForEach-Object {
            if (-not $files.Contains($_.FullName)) { [void]$files.Add($_.FullName) }
        }
    }
    return @($files)
}

function Test-LogsContain([string[]]$Patterns, [string]$Needle, [switch]$Regex, [switch]$ExactLine) {
    foreach ($path in @(Get-LogFiles $Patterns)) {
        $text = [string](Get-Text $path)
        if ($Regex) {
            if ($text -match $Needle) { return $true }
        } elseif ($ExactLine) {
            if (@($text -split '\r?\n') -contains $Needle) { return $true }
        } elseif ($text.Contains($Needle)) {
            return $true
        }
    }
    return $false
}

function Write-RoleConfig(
    [string]$Path,
    [string]$NodeId,
    [string]$MeshEndpoint,
    [string]$StreamEndpoint,
    [string]$HttpEndpoint,
    [string]$RedisEndpoint,
    [string]$RedisKeyPrefix,
    [string]$BroadcastEndpoint,
    [string]$MeshAdvertiseHost = "",
    [bool]$SubscriberOnly = $false,
    [bool]$DisableBots = $false,
    [bool]$AllowEmptyZoneSet = $false
) {
    $zoneworld = @{
        redisEndpoint = $RedisEndpoint
        redisKeyPrefix = $RedisKeyPrefix
        nodeId = $NodeId
        meshEndpoint = $MeshEndpoint
        streamEndpoint = $StreamEndpoint
        broadcastEndpoint = $BroadcastEndpoint
        bootstrapHttpEndpoint = $HttpEndpoint
        logDir = $LogDir
        faultTickZone = if ($NodeId.StartsWith("zone-node-")) { "zone-nw" } else { $null }
        subscriberOnly = $SubscriberOnly
        disableBots = $DisableBots
        allowEmptyZoneSet = $AllowEmptyZoneSet
    }
    if ($MeshAdvertiseHost) { $zoneworld.meshAdvertiseHost = $MeshAdvertiseHost }
    @{ sample = @{ zoneworld = $zoneworld } } | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 -LiteralPath $Path
}

function Start-ZoneNode([string]$Name, [string]$ConfigName = "", [string]$WaitEndpoint = "") {
    if (-not $ConfigName) { $ConfigName = $Name }
    if (-not $WaitEndpoint) {
        if ($Name -eq "zone-node-1") { $WaitEndpoint = $Node1Mesh }
        elseif ($Name -eq "zone-node-2") { $WaitEndpoint = $Node2Mesh }
    }
    [void](Start-Role $Name $ZoneNodeBin @("--config=$(Join-Path $ConfigDir "$ConfigName.json")"))
    if ($WaitEndpoint) { Wait-Port "$Name mesh" $WaitEndpoint }
    if (-not (Wait-RoleLog $Name "topology=ready node=$Name zones=" 30)) {
        throw "$Name did not report topology readiness."
    }
}

function Invoke-ClientLane([string]$Name, [string[]]$Arguments, [int]$TimeoutMilliseconds = 180000) {
    $allArguments = @("--game-endpoint", $GameStream, "--ops-endpoint", $OpsStream) + $Arguments
    [void](Start-Role "client-$Name" $ClientBin $allArguments)
    return (Wait-RoleExit "client-$Name" $TimeoutMilliseconds)
}

function Get-RoutingReports([string]$NodeId) {
    $reports = New-Object System.Collections.Generic.List[object]
    foreach ($path in @(Get-LogFiles @("ops-*.stdout.log", "ops.log"))) {
        foreach ($line in @((Get-Text $path) -split '\r?\n')) {
            if ($line -match "zoneworld-node-report node=$([regex]::Escape($NodeId)) rid=([^ ]+) registered=(true|false)") {
                [void]$reports.Add([pscustomobject]@{ Rid = $Matches[1]; Registered = $Matches[2] })
            }
        }
    }
    return $reports.ToArray()
}

function Get-RoutingId([string]$NodeId) {
    $reports = @(Get-RoutingReports $NodeId)
    if ($reports.Count -eq 0) { return "" }
    return [string]$reports[-1].Rid
}

function Wait-NewRoutingId([string]$NodeId, [string]$Previous, [int]$TimeoutSeconds = 30) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $reports = @(Get-RoutingReports $NodeId)
        $candidate = if ($reports.Count -gt 0) { [string]$reports[-1].Rid } else { "" }
        $observations = @($reports | Where-Object { $_.Rid -eq $candidate -and $_.Registered -eq "true" }).Count
        if ($candidate -and $candidate -ne $Previous -and $observations -ge 2) { return $candidate }
        Start-Sleep -Milliseconds 100
    }
    return ""
}

function Invoke-Child([string]$Switch) {
    $powershell = Get-ZlinkSampleSelfShellPath
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $powershell -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath $Switch
    $childStatus = $LASTEXITCODE
    $ErrorActionPreference = $previousPreference
    if ($childStatus -ne 0) { throw "ZoneWorld child lane $Switch failed with exit code $childStatus." }
}

function Record-Verdict([string]$Id, [string]$Verdict, [string]$Detail) {
    $Verdicts[$Id] = $Verdict
    $Details[$Id] = $Detail
}

function Show-FailureLogs {
    foreach ($path in @(Get-LogFiles @("*.log"))) {
        Write-Warning "===== $path"
        Get-Content -LiteralPath $path -Tail 80 -ErrorAction SilentlyContinue | ForEach-Object { Write-Warning $_ }
    }
}

$RedisContainer = $null
$Status = 1
try {
    & cmake --build $BuildDir --config $BuildConfiguration --parallel 2 --target `
        sample_cpp_framework_zoneworld_zone_node `
        sample_cpp_framework_zoneworld_gateway `
        sample_cpp_framework_zoneworld_ops `
        sample_cpp_framework_zoneworld_client
    if ($LASTEXITCODE -ne 0) { throw "ZoneWorld sample build failed with exit code $LASTEXITCODE." }

    if (-not $B8Child -and -not $G4Child) {
        Invoke-Child "-B8Child"
        $B8Proven = $true
        Invoke-Child "-G4Child"
        $G4Proven = $true
    } else {
        $B8Proven = $false
        $G4Proven = $false
    }

    $ZoneNodeBin = Find-Binary "sample_cpp_framework_zoneworld_zone_node"
    $GatewayBin = Find-Binary "sample_cpp_framework_zoneworld_gateway"
    $OpsBin = Find-Binary "sample_cpp_framework_zoneworld_ops"
    $ClientBin = Find-Binary "sample_cpp_framework_zoneworld_client"

    $ports = @(Get-ZlinkSamplePorts -Count 16)
    $Node1Mesh = "tcp://127.0.0.1:$($ports[0])"
    $Node2Mesh = "tcp://127.0.0.1:$($ports[1])"
    $GatewayMesh = "tcp://127.0.0.1:$($ports[2])"
    $OpsMesh = "tcp://127.0.0.1:$($ports[3])"
    $GameStream = "tcp://127.0.0.1:$($ports[4])"
    $OpsStream = "tcp://127.0.0.1:$($ports[5])"
    $Broadcast = "tcp://127.0.0.1:$($ports[6])"
    $Node1Http = "http://127.0.0.1:$($ports[7])"
    $Node2Http = "http://127.0.0.1:$($ports[8])"
    $GatewayHttp = "http://127.0.0.1:$($ports[9])"
    $Node1Stream = "tcp://127.0.0.1:$($ports[10])"
    $Node2Stream = "tcp://127.0.0.1:$($ports[11])"
    $OpsHttp = "http://127.0.0.1:$($ports[12])"
    $Node3Mesh = "tcp://127.0.0.1:$($ports[13])"
    $Node3Stream = "tcp://127.0.0.1:$($ports[14])"
    $Node3Http = "http://127.0.0.1:$($ports[15])"

    $GatewayMeshBind = $GatewayMesh
    $Node1MeshBind = $Node1Mesh
    $Node2MeshBind = $Node2Mesh
    $MeshAdvertiseHost = ""
    if ($B8Child) {
        $GatewayMeshBind = "tcp://127.0.0.2:$($ports[2])"
        $Node1MeshBind = "tcp://127.0.0.2:$($ports[0])"
        $Node2MeshBind = "tcp://127.0.0.2:$($ports[1])"
        $MeshAdvertiseHost = "127.0.0.1"
    }

    $redis = Start-ZlinkSampleRedis "zlink-redis-cpp-sample-zoneworld" "redis:7-alpine"
    $RedisContainer = $redis.ContainerId
    $RedisEndpoint = "tcp://$($redis.Endpoint)"
    Wait-Port "redis" $RedisEndpoint
    $KeyPrefix = "zoneworld:cpp:${PID}:$([Guid]::NewGuid().ToString('N')):"

    Write-RoleConfig (Join-Path $ConfigDir "zone-node-1.json") "zone-node-1" $Node1MeshBind $Node1Stream $Node1Http $RedisEndpoint $KeyPrefix $Broadcast $MeshAdvertiseHost
    Write-RoleConfig (Join-Path $ConfigDir "zone-node-2.json") "zone-node-2" $Node2MeshBind $Node2Stream $Node2Http $RedisEndpoint $KeyPrefix $Broadcast $MeshAdvertiseHost
    Write-RoleConfig (Join-Path $ConfigDir "zone-node-1-replacement.json") "zone-node-1" $Node1MeshBind $Node1Stream $Node1Http $RedisEndpoint $KeyPrefix $Broadcast $MeshAdvertiseHost $false $true $true
    Write-RoleConfig (Join-Path $ConfigDir "zone-node-2-replacement.json") "zone-node-2" $Node2MeshBind $Node2Stream $Node2Http $RedisEndpoint $KeyPrefix $Broadcast $MeshAdvertiseHost $false $true $true
    Write-RoleConfig (Join-Path $ConfigDir "ops.json") "ops" $OpsMesh $OpsStream $OpsHttp $RedisEndpoint $KeyPrefix $Broadcast
    Write-RoleConfig (Join-Path $ConfigDir "gateway.json") "gateway" $GatewayMeshBind $GameStream $GatewayHttp $RedisEndpoint $KeyPrefix $Broadcast $MeshAdvertiseHost
    Write-RoleConfig (Join-Path $ConfigDir "zone-node-3.json") "zone-node-3" $Node3Mesh $Node3Stream $Node3Http $RedisEndpoint $KeyPrefix $Broadcast "" $true $true

    if ($B8Child) {
        $python = Get-ZlinkSamplePythonCommand
        if ($python) {
            $proxyRuntime = $python.Path
            $proxyPrefix = @($python.Arguments)
            $proxyScript = Join-Path $PSScriptRoot "Support/session_route_block_proxy.py"
        } else {
            $node = Get-Command node.exe -ErrorAction SilentlyContinue | Select-Object -First 1
            if (-not $node) { throw "Python or Node.js is required for the B8 proxy." }
            $proxyRuntime = $node.Source
            $proxyPrefix = @()
            $proxyScript = Join-Path $PSScriptRoot "Support/session_route_block_proxy.mjs"
        }
        for ($proxyIndex = 0; $proxyIndex -lt 3; $proxyIndex++) {
            $proxyName = "session-route-proxy-$proxyIndex"
            $proxyArguments = $proxyPrefix + @(
                $proxyScript,
                "--listen-host", "127.0.0.1", "--listen-port", [string]$ports[$proxyIndex],
                "--target-host", "127.0.0.2", "--target-port", [string]$ports[$proxyIndex],
                "--arm-file", (Join-Path $RunDir "b8-block-command-44"))
            [void](Start-Role $proxyName $proxyRuntime $proxyArguments)
            if (-not (Wait-RoleLog $proxyName "proxy-ready" 30)) { throw "$proxyName did not become ready." }
        }
        Start-ZoneNode "zone-node-1"
        Start-ZoneNode "zone-node-2"
    } else {
        Start-ZoneNode "zone-node-2"
        Start-ZoneNode "zone-node-1"
    }
    [void](Start-Role "ops" $OpsBin @("--config=$(Join-Path $ConfigDir 'ops.json')"))
    Wait-Port "ops stream" $OpsStream
    [void](Start-Role "gateway" $GatewayBin @("--config=$(Join-Path $ConfigDir 'gateway.json')"))
    Wait-Port "gateway stream" $GameStream

    Start-Sleep -Seconds 2
    Invoke-WebRequest -UseBasicParsing -Method Post -ContentType "application/json" -Body "{}" `
        -Uri "$GatewayHttp/bootstrap-world" -TimeoutSec 30 | Out-Null
    Start-Sleep -Seconds 6

    if ($B8Child) {
        $armFile = Join-Path $RunDir "b8-block-command-44"
        [void](Start-Role "client-b8" $ClientBin @(
            "--game-endpoint", $GameStream, "--ops-endpoint", $OpsStream,
            "--scenario", "B8", "--arm-file", $armFile))
        if (-not (Wait-RoleLog "client-b8" "scenario ZW-B8 armed" 60)) { throw "zoneworld-b8=failed reason=client-not-armed" }
        New-Item -ItemType File -Path $armFile | Out-Null
        $blockedDeadline = [DateTime]::UtcNow.AddSeconds(60)
        while ([DateTime]::UtcNow -lt $blockedDeadline -and
            -not (Test-LogsContain @("session-route-proxy-*.stdout.log") "blocked-command-44")) {
            Start-Sleep -Milliseconds 100
        }
        if (-not (Test-LogsContain @("session-route-proxy-*.stdout.log") "blocked-command-44")) {
            throw "zoneworld-b8=failed reason=command-44-not-blocked"
        }
        $blockedFile = "$armFile.blocked"
        if (-not (Test-Path -LiteralPath $blockedFile)) { New-Item -ItemType File -Path $blockedFile | Out-Null }
        $b8Status = Wait-RoleExit "client-b8" 180000
        if ($b8Status -ne 0 -or
            -not (Test-LogsContain @("client-b8-*.stdout.log") "scenario ZW-B8 passed") -or
            -not (Test-LogsContain @("session-route-proxy-*.stdout.log") "blocked-command-44")) {
            throw "zoneworld-b8=failed reason=seal-timeout-or-reconnect"
        }
        Write-Host "scenario ZW-B8 passed"
        Write-Host "zoneworld-b8=completed"
        $Status = 0
        return
    }

    if ($G4Child) {
        [void](Start-Role "client-g4" $ClientBin @("--game-endpoint", $GameStream, "--ops-endpoint", $OpsStream, "--scenario", "G4"))
        if (-not (Wait-RoleLog "client-g4" "scenario ZW-G4 armed node=" 60)) { throw "zoneworld-g4=failed reason=client-not-armed" }
        $g4Log = Get-Text ([string]$RoleStdout["client-g4"])
        if ($g4Log -notmatch 'scenario ZW-G4 armed node=([^ ]+)') { throw "zoneworld-g4=failed reason=target-not-reported" }
        $g4Node = $Matches[1].Trim()
        if ($g4Node -ne "zone-node-1" -and $g4Node -ne "zone-node-2") { throw "zoneworld-g4=failed reason=invalid-target" }
        $g4OldRid = Get-RoutingId $g4Node
        if (-not (Wait-RoleLog $g4Node "zoneworld-crash-boundary join pending" 60)) { throw "zoneworld-g4=failed reason=crash-boundary-not-reached" }
        Stop-Role $g4Node
        $g4ClientStatus = Wait-RoleExit "client-g4" 180000
        Start-ZoneNode $g4Node "$g4Node-replacement"
        $g4NewRid = Wait-NewRoutingId $g4Node $g4OldRid
        $g4FreshStatus = Invoke-ClientLane "g4-fresh" @("--scenario", "G4-fresh")
        if ($g4ClientStatus -ne 0 -or $g4FreshStatus -ne 0 -or $g4NewRid -notmatch '^zn-[0-9a-f-]{36}$' -or
            -not (Test-LogsContain @("client-g4-*.stdout.log") "scenario ZW-G4-boundary passed") -or
            -not (Test-LogsContain @("client-g4-fresh-*.stdout.log") "fresh-actor-proof scenario=G4-fresh") -or
            -not (Test-LogsContain @("client-g4-fresh-*.stdout.log") "owner=$g4NewRid")) {
            throw "zoneworld-g4=failed reason=boundary-or-fresh-actor-proof"
        }
        Write-Host "scenario ZW-G4 passed"
        Write-Host "zoneworld-g4=completed"
        $Status = 0
        return
    }

    $Verdicts = @{}
    $Details = @{}
    $clientStatus = Invoke-ClientLane "main" @()

    $mainNodeLogs = @(Get-LogFiles @("zone-node-1*.log", "zone-node-2*.log"))
    $mainBotJoinFailures = 0
    $mainBotRelocation = $false
    foreach ($path in $mainNodeLogs) {
        $text = Get-Text $path
        $mainBotJoinFailures += ([regex]::Matches($text, 'zoneworld-join-failed player=bot-')).Count
        if ($text -match 'zoneworld-actor-joined.*player=bot-.*initial=false') { $mainBotRelocation = $true }
    }

    Start-ZoneNode "zone-node-3" "zone-node-3"
    Start-Sleep -Seconds 1
    $d2ClientStatus = Invoke-ClientLane "d2" @("--scenario", "D2")
    $d2Status = $false
    if ($d2ClientStatus -eq 0) {
        $d2Text = Get-Text ([string]$RoleStdout["client-d2"])
        if ($d2Text -match '(?m)^announcement-proof id=(.+)$') {
            $announcementId = $Matches[1].Trim()
            $d2Status = Test-LogsContain @("zone-node-3*.log") "zoneworld-fanout-announcement node=zone-node-3 id=$announcementId"
        }
    }

    $transitionStatus = $false
    $transitionNode = ""
    [void](Start-Role "client-transition" $ClientBin @("--game-endpoint", $GameStream, "--ops-endpoint", $OpsStream, "--scenario", "transition"))
    if (Wait-RoleLog "client-transition" "scenario ZW-B4-C3 armed node=" 60) {
        $transitionText = Get-Text ([string]$RoleStdout["client-transition"])
        if ($transitionText -match 'scenario ZW-B4-C3 armed node=([^ ]+)') { $transitionNode = $Matches[1].Trim() }
        if ($transitionNode -eq "zone-node-1" -or $transitionNode -eq "zone-node-2") {
            Stop-Role $transitionNode
            $transitionClientStatus = Wait-RoleExit "client-transition" 180000
            $transitionStatus = $transitionClientStatus -eq 0 -and
                (Test-LogsContain @("client-transition-*.stdout.log") "scenario ZW-B4 passed") -and
                (Test-LogsContain @("client-transition-*.stdout.log") "scenario ZW-C3 passed")
            Start-ZoneNode $transitionNode "$transitionNode-replacement"
            Start-Sleep -Seconds 2
        }
    }

    $e5Status = $false
    $e5Node = "zone-node-2"
    $e5ArmStatus = Invoke-ClientLane "e5-arm" @("--scenario", "E5-arm", "--target-node-id", $e5Node)
    if ($e5ArmStatus -eq 0) {
        [void](Start-Role "client-e5" $ClientBin @(
            "--game-endpoint", $GameStream, "--ops-endpoint", $OpsStream,
            "--scenario", "E5", "--target-node-id", $e5Node))
        if (Wait-RoleLog "client-e5" "scenario ZW-E5 restore armed" 60) {
            Stop-Role $e5Node
            if (Wait-RoleLog "client-e5" "scenario ZW-E5 replacement waiting" 60) {
                Start-ZoneNode $e5Node "$e5Node-replacement"
            }
        }
        $e5ClientStatus = Wait-RoleExit "client-e5" 180000
        $e5Status = $e5ClientStatus -eq 0 -and (Test-LogsContain @("client-e5-*.stdout.log") "scenario ZW-E5 passed")
    }

    $g3Status = $false
    $g3Node = "zone-node-1"
    $g3OldRid = Get-RoutingId $g3Node
    if ($g3OldRid) {
        Stop-Role $g3Node
        Start-ZoneNode $g3Node "$g3Node-replacement"
        $g3NewRid = Wait-NewRoutingId $g3Node $g3OldRid
        if ($g3NewRid) {
            $g3ClientStatus = Invoke-ClientLane "g3" @("--scenario", "G3")
            $g3Status = $g3ClientStatus -eq 0 -and $g3NewRid -match '^zn-[0-9a-f-]{36}$' -and
                (Test-LogsContain @("client-g3-*.stdout.log") "fresh-actor-proof scenario=G3") -and
                (Test-LogsContain @("client-g3-*.stdout.log") "owner=$g3NewRid")
        }
    }

    $g2Node1Rid = Get-RoutingId "zone-node-1"
    $g2Node2Rid = Get-RoutingId "zone-node-2"
    $g2Status = $g2Node1Rid -match '^zn-[0-9a-f-]{36}$' -and $g2Node2Rid -match '^zn-[0-9a-f-]{36}$' -and $g2Node1Rid -ne $g2Node2Rid

    [void](Start-Role "client-c2" $ClientBin @(
        "--game-endpoint", $GameStream, "--ops-endpoint", $OpsStream,
        "--scenario", "C2", "--target-node-id", "zone-node-2"))
    if (Wait-RoleLog "client-c2" "scenario ZW-C2 armed node=" 60) {
        Stop-Role "zone-node-2"
        [void](Wait-RoleExit "client-c2" 180000)
        Start-ZoneNode "zone-node-2" "zone-node-2-replacement"
    }

    foreach ($id in $ExpectedIds) {
        if (Test-LogsContain @("client-*.stdout.log") "scenario $id passed" -ExactLine) {
            Record-Verdict $id "PASS" "typed client assertion"
        }
    }
    if ($B8Proven) { Record-Verdict "ZW-B8" "PASS" "command-44 seal timeout and reconnect" }
    if ($d2Status) { Record-Verdict "ZW-D2" "PASS" "third subscriber received reannounce" }
    if ($transitionStatus) {
        Record-Verdict "ZW-B4" "PASS" "border snapshot expired after publisher stop"
        Record-Verdict "ZW-C3" "PASS" "report TTL expiry observed"
    }
    if ($e5Status) { Record-Verdict "ZW-E5" "PASS" "maintenance restored after restart" }
    if ($g2Status) { Record-Verdict "ZW-G2" "PASS" "reverse startup order remained routable" }
    if ($g3Status) { Record-Verdict "ZW-G3" "PASS" "normal replacement advanced RID and accepted fresh object" }
    if ($G4Proven) { Record-Verdict "ZW-G4" "PASS" "crash boundary stayed Unavailable and replacement accepted fresh object" }

    $gatewayText = [string]((@(Get-LogFiles @("gateway*.log")) | ForEach-Object { Get-Text $_ }) -join "`n")
    $spawned = @([regex]::Matches($gatewayText, 'zoneworld-bot-spawned bot=([^ ]+)') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique).Count
    if ($spawned -eq 8 -and
        $gatewayText.Contains('bot=bot-nw-x zone=zone-nw start=10,15 dir=1,0') -and
        $gatewayText.Contains('bot=bot-se-y zone=zone-se start=85,90 dir=0,-1')) {
        Record-Verdict "ZW-F1" "PASS" "eight fixed bots and endpoint trajectories observed"
    } else {
        Record-Verdict "ZW-F1" "FAIL" "expected eight canonical bot spawns, observed $spawned"
    }
    if (-not $mainBotRelocation) {
        Record-Verdict "ZW-F2" "FAIL" "no unbound bot relocation completion"
    } elseif ($mainBotJoinFailures -ne 0) {
        Record-Verdict "ZW-F2" "FAIL" "bot relocation completed only behind $mainBotJoinFailures join failures"
    } else {
        Record-Verdict "ZW-F2" "PASS" "unbound bot relocation completed with no join failure"
    }
    if (Test-LogsContain @("zone-node-*.log") 'zoneworld-bot-reversed player=bot-.*reason=(OutOfRange|TooFar)' -Regex) {
        Record-Verdict "ZW-F3" "PASS" "bot direction reversal observed at rejection"
    } else {
        Record-Verdict "ZW-F3" "FAIL" "bot rejection did not expose a direction reversal"
    }
    if (-not (Test-LogsContain @("*.log") 'bound_session_push actor=bot-|push-to-bot' -Regex)) {
        Record-Verdict "ZW-F4" "PASS" "no bound-session push was attempted for a bot"
    } else {
        Record-Verdict "ZW-F4" "FAIL" "a bot received a bound-session push attempt"
    }

    $zoneNodeSource = [string](Get-Text (Join-Path $PSScriptRoot "Server/ZoneNode/main.cpp"))
    if ($zoneNodeSource.Contains('set_automatic_routing_id_prefix ("zn")') -and $zoneNodeSource -notmatch 'set_routing_id *\(') {
        Record-Verdict "ZW-G5" "PASS" "ZoneNode uses generated routing ids only"
    } else {
        Record-Verdict "ZW-G5" "FAIL" "fixed or non-canonical ZoneNode routing configuration"
    }

    if (-not $Verdicts.ContainsKey("ZW-A1") -and $clientStatus -ne 0 -and
        (Test-LogsContain @("zone-node-*.log") "zoneworld-join-accepted player=player-alice") -and
        (Test-LogsContain @("client-main-*.stderr.log") "stream connector wait timed out")) {
        Record-Verdict "ZW-A1" "BLOCKED" "Framework deferred admission completed but JoinWorldRes/bound push did not commit; run-dir=$RunDir"
    }
    if ($Verdicts.ContainsKey("ZW-A1") -and $Verdicts["ZW-A1"] -eq "PASS" -and
        $Verdicts.ContainsKey("ZW-A4") -and $Verdicts["ZW-A4"] -eq "PASS" -and
        $Verdicts.ContainsKey("ZW-A5") -and $Verdicts["ZW-A5"] -eq "PASS" -and
        -not $Verdicts.ContainsKey("ZW-A2") -and $clientStatus -ne 0) {
        $clientTail = @(Get-Text ([string]$RoleStdout["client-main"]) -split '\r?\n') | Select-Object -Last 1
        if ($clientTail -like '*stream connector wait timed out*') {
            Record-Verdict "ZW-A2" "FAIL" "same-zone MoveMsg did not produce the awaited ZoneStateNotify; run-dir=$RunDir"
        }
    }
    foreach ($id in @("ZW-A2", "ZW-A3", "ZW-A4", "ZW-A5", "ZW-B1", "ZW-B2", "ZW-B3", "ZW-B4", "ZW-B5", "ZW-B6", "ZW-B7", "ZW-B8", "ZW-D1", "ZW-E2", "ZW-E3", "ZW-E4")) {
        if (-not $Verdicts.ContainsKey($id)) {
            if (-not $Verdicts.ContainsKey("ZW-A1") -or $Verdicts["ZW-A1"] -ne "PASS") {
                Record-Verdict $id "BLOCKED" "depends on ZW-A1 bound-session readiness"
            } else {
                Record-Verdict $id "BLOCKED" "not reached after the first later scenario failure"
            }
        }
    }
    if (-not $Verdicts.ContainsKey("ZW-C4")) {
        if ((Test-LogsContain @("zone-node-*.log") "zlink.runtime.spot.timer_failed") -and
            (Test-LogsContain @("ops*.log") "zoneworld-node-alert ")) {
            Record-Verdict "ZW-C4" "PASS" "public Spot timer event reached the Ops handler"
        } else {
            Record-Verdict "ZW-C4" "FAIL" "public Spot timer event did not reach the Ops handler"
        }
    }
    foreach ($id in @("ZW-C2", "ZW-C3", "ZW-D2", "ZW-E5", "ZW-G2", "ZW-G3", "ZW-G4")) {
        if (-not $Verdicts.ContainsKey($id)) { Record-Verdict $id "FAIL" "runner-driven lifecycle lane was not reached" }
    }

    $allPassed = $true
    foreach ($id in $ExpectedIds) {
        $verdict = if ($Verdicts.ContainsKey($id)) { [string]$Verdicts[$id] } else { "FAIL" }
        $detail = if ($Details.ContainsKey($id)) { [string]$Details[$id] } else { "no verdict was recorded" }
        Write-Host "scenario-ledger id=$id verdict=$verdict detail=$detail"
        if ($verdict -ne "PASS") { $allPassed = $false }
    }
    if (-not $allPassed) {
        Write-Host "zoneworld=blocked run-dir=$RunDir"
        throw "One or more ZoneWorld scenarios failed."
    }

    Write-Host "zoneworld=completed"
    Write-Host "PASS ZoneWorld.Cpp"
    Write-Host "zoneworld sample result=passed"
    $Status = 0
} catch {
    [Console]::Error.WriteLine("ZoneWorld runner failed: $($_.Exception.Message)")
    [Console]::Error.WriteLine("at $($_.InvocationInfo.ScriptName):$($_.InvocationInfo.ScriptLineNumber)")
    Show-FailureLogs
    throw
} finally {
    for ($index = $Processes.Count - 1; $index -ge 0; $index--) {
        $process = $Processes[$index]
        try {
            if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
            [void]$process.WaitForExit(1000)
        } catch {
        }
    }
    if ($RedisContainer) { Remove-ZlinkSampleRedis $RedisContainer }
    Close-ZlinkSampleRunDir -RunDir $RunDir -Status $Status -Label "ZoneWorld"
}

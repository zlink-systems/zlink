[CmdletBinding(PositionalBinding = $false)]
param(
    [switch]$Local,
    [switch]$NoBuild,
    [switch]$BuildOnly,
    [switch]$CheckStructure,
    [switch]$CheckDomain,
    [string]$Scenario,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$RunnerArguments = @()
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = $PSScriptRoot
. (Join-Path $root 'scripts/run-helpers.ps1')
Assert-PowerShellVersion
Set-Location $root
New-Item -ItemType Directory -Force (Join-Path $root '.logs') | Out-Null
if ($CheckStructure -and $CheckDomain) { throw 'Choose one domain or structure check.' }
if ($CheckStructure) { $RunnerArguments = @('--check-structure') + $RunnerArguments }
elseif ($CheckDomain) { $RunnerArguments = @('--check-domain') + $RunnerArguments }
elseif ($Scenario) { $RunnerArguments = @('--scenario', $Scenario) + $RunnerArguments }
$mode = if ($RunnerArguments.Count -gt 0) { $RunnerArguments[0] } else { '' }
$checks = @(if (Test-Path -LiteralPath 'e2e/checks.conf') { Get-Content -LiteralPath 'e2e/checks.conf' })
# A scenario run uses the server started by run-server.ps1 when one is running.
if (-not $Local -and -not $BuildOnly -and $mode -ne '--attach' -and
    $mode -notlike '--check-*' -and $mode -ne '--domain-checks-only' -and (Test-RunningServer -Root $root)) {
    Write-Host 'Using the running server (logs/server.json)'
    & node dist/e2e/runner.js --attach @RunnerArguments
    exit $LASTEXITCODE
}
if (-not $Local -and $mode -eq '--attach') {
    & node dist/e2e/runner.js @RunnerArguments
    exit $LASTEXITCODE
}
if ($Local) {
    if (-not $NoBuild) {
        Assert-NoRunningServer -Root $root
        $buildLog = Join-Path $root '.logs/build.log'
        $code = Invoke-Logged -Log $buildLog -Command npm -Arguments @('ci')
        if ($code -ne 0) { exit $code }
        $code = Invoke-Logged -Log $buildLog -Append -Command npm -Arguments @('run', 'build')
        if ($code -ne 0) {
            Get-Content (Join-Path $root '.logs/build.log') -Tail 20 | Write-Host
            exit 1
        }
    }
    if ($BuildOnly) {
        foreach ($line in $checks) {
            $parts = @($line -split '\s+')
            if ($parts[0] -eq 'build' -and $parts[1] -eq 'runner') {
                $arguments = $parts[2..($parts.Length - 1)]
                & node dist/e2e/runner.js @arguments
                if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
            }
        }
        exit 0
    }
    & node dist/e2e/runner.js @RunnerArguments
    exit $LASTEXITCODE
}
if ($NoBuild) { throw '-NoBuild requires -Local; Docker builds reuse the layer cache automatically.' }
$project = 'my-game-e2e-' + [guid]::NewGuid().ToString('N')
$compose = @('compose', '-f', (Join-Path $root 'docker-compose.yml'), '-p', $project)
$profiles = $env:COMPOSE_PROFILES
try {
    $env:COMPOSE_PROFILES = ''
    $redisServices = @(& docker @compose config --services)
    if ($LASTEXITCODE -ne 0) { throw 'Compose service discovery failed.' }
}
finally { $env:COMPOSE_PROFILES = $profiles }
$compose += @('--profile', 'docker')
$logs = Join-Path $root ".logs/docker-$project"
New-Item -ItemType Directory $logs | Out-Null
# BuildKit reuses the previous transfer of a build context with the same directory name and
# compares only file size and modification time, so two projects named my-game with equal
# timestamps would build each other's files. A link with a per-run name gives every build its own
# transfer.
$env:MY_GAME_BUILD_CONTEXT = Join-Path $logs $project
$linkType = if (Test-OnWindows) { 'Junction' } else { 'SymbolicLink' }
New-Item -ItemType $linkType -Path $env:MY_GAME_BUILD_CONTEXT -Target $root | Out-Null
function Invoke-Checks([string]$Phase) {
    foreach ($line in $checks) {
        $parts = @($line -split '\s+')
        if ($parts[0] -eq $Phase) {
            $checkArguments = $parts[1..($parts.Length - 1)]
            & docker @compose run --rm --no-deps -T @checkArguments
            if ($LASTEXITCODE -ne 0) { throw "Docker $Phase check failed." }
        }
    }
}
$result = 1
Write-Host "Docker logs: $logs"
try {
    $code = Invoke-Logged -Log (Join-Path $logs 'build.log') -Command docker -Arguments ($compose + @('build'))
    if ($code -ne 0) {
        Get-Content (Join-Path $logs 'build.log') -Tail 30 | Write-Host
        throw 'Docker image build failed.'
    }
    if ($BuildOnly) {
        Invoke-Checks build
        $result = 0
    }
    elseif ($mode -eq '--check-structure' -or $mode -eq '--domain-checks-only') {
        & docker @compose run --rm --no-deps -T --name "$project-check" runner @RunnerArguments
        $result = $LASTEXITCODE
    }
    elseif ($mode -eq '--check-domain') {
        if (@($checks -like 'preflight *').Count -gt 0) {
            Invoke-Checks preflight
            $result = 0
        }
        else {
            & docker @compose run --rm --no-deps -T --name "$project-domain" runner @RunnerArguments
            $result = $LASTEXITCODE
        }
    }
    else {
        & docker @compose run --rm --no-deps -T --name "$project-check" runner --check-structure
        if ($LASTEXITCODE -ne 0) { throw 'Structure check failed.' }
        Invoke-Checks preflight
        $code = Invoke-Logged -Log (Join-Path $logs 'redis-start.log') -Command docker -Arguments ($compose + @('up', '-d', '--wait', '--wait-timeout', '30') + $redisServices)
        if ($code -ne 0) { throw 'Redis startup failed.' }
        & docker @compose run --rm --no-deps -T --name "$project-control" runner @RunnerArguments
        $result = $LASTEXITCODE
    }
}
catch { Write-Host $_ -ForegroundColor Red }
finally {
    $containers = @(& docker ps -aq --filter "label=my-game.e2e=$project")
    if ($LASTEXITCODE -ne 0) { $result = 1 }
    if ($containers.Count -gt 0) {
        $code = Invoke-Logged -Log (Join-Path $logs 'containers-stop.log') -Command docker -Arguments (@('rm', '-f') + $containers)
        if ($code -ne 0) { $result = 1 }
    }
    $code = Invoke-Logged -Log (Join-Path $logs 'redis.log') -Command docker -Arguments ($compose + @('logs', '--no-color') + $redisServices)
    if ($code -ne 0) { $result = 1 }
    $code = Invoke-Logged -Log (Join-Path $logs 'cleanup.log') -Command docker -Arguments ($compose + @('down', '--volumes'))
    if ($code -ne 0) { $result = 1 }
    foreach ($image in @("$project-node", "$project-runner")) {
        if ((Invoke-Logged -Command docker -Arguments @('image', 'inspect', $image)) -eq 0) {
            $code = Invoke-Logged -Log (Join-Path $logs 'cleanup.log') -Append -Command docker -Arguments @('image', 'rm', $image)
            if ($code -ne 0) { $result = 1 }
        }
    }
    [IO.Directory]::Delete($env:MY_GAME_BUILD_CONTEXT)
}
exit $result

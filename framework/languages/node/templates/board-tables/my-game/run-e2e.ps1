[CmdletBinding(PositionalBinding = $false)]
param(
    [switch]$Local,
    [switch]$NoBuild,
    [switch]$BuildOnly,
    [switch]$CheckStructure,
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
if ($CheckStructure) { $RunnerArguments = @('--check-structure') + $RunnerArguments }
elseif ($Scenario) { $RunnerArguments = @('--scenario', $Scenario) + $RunnerArguments }
# A scenario run uses the server started by run-server.ps1 when one is running.
if (-not $Local -and -not $BuildOnly -and -not $CheckStructure -and (Test-RunningServer -Root $root)) {
    Write-Host 'Using the running server (logs/server.json)'
    & node dist/e2e/runner.js --attach @RunnerArguments
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
        & node dist/e2e/runner.js --domain-checks-only
        exit $LASTEXITCODE
    }
    & node dist/e2e/runner.js @RunnerArguments
    exit $LASTEXITCODE
}
if ($NoBuild) { throw '-NoBuild requires -Local; Docker builds reuse the layer cache automatically.' }
$project = 'my-game-e2e-' + [guid]::NewGuid().ToString('N')
$logs = Join-Path $root ".logs/docker-$project"
New-Item -ItemType Directory $logs | Out-Null
# BuildKit reuses the previous transfer of a build context with the same directory name and
# compares only file size and modification time, so two projects named my-game with equal
# timestamps would build each other's files. A link with a per-run name gives every build its own
# transfer.
$env:MY_GAME_BUILD_CONTEXT = Join-Path $logs $project
$linkType = if (Test-OnWindows) { 'Junction' } else { 'SymbolicLink' }
New-Item -ItemType $linkType -Path $env:MY_GAME_BUILD_CONTEXT -Target $root | Out-Null
$compose = @('compose', '-f', (Join-Path $root 'docker-compose.yml'), '-p', $project, '--profile', 'docker')
$result = 1
Write-Host "Docker logs: $logs"
try {
    $code = Invoke-Logged -Log (Join-Path $logs 'build.log') -Command docker -Arguments ($compose + @('build'))
    if ($code -ne 0) {
        Get-Content (Join-Path $logs 'build.log') -Tail 30 | Write-Host
        throw 'Docker image build failed.'
    }
    if ($BuildOnly) {
        & docker @compose run --rm --no-deps -T --name "$project-domain" runner --domain-checks-only
        $result = $LASTEXITCODE
    }
    else {
        $checkArguments = @(if ($CheckStructure) { $RunnerArguments } else { '--check-structure' })
        & docker @compose run --rm --no-deps -T --name "$project-check" runner @checkArguments
        if ($LASTEXITCODE -ne 0) { throw 'Structure check failed.' }
        if ($CheckStructure) { $result = 0 }
        else {
            $code = Invoke-Logged -Log (Join-Path $logs 'redis-start.log') -Command docker -Arguments ($compose + @('up', '-d', '--wait', '--wait-timeout', '30', 'redis'))
            if ($code -ne 0) { throw 'Redis startup failed.' }
            & docker @compose run --rm --no-deps -T --name "$project-control" runner @RunnerArguments
            $result = $LASTEXITCODE
        }
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
    $code = Invoke-Logged -Log (Join-Path $logs 'redis.log') -Command docker -Arguments ($compose + @('logs', '--no-color', 'redis'))
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

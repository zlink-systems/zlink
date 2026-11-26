[CmdletBinding(PositionalBinding = $false)]
param(
    [switch]$Local,
    [switch]$Attach,
    [switch]$NoBuild,
    [switch]$BuildOnly,
    [switch]$CheckStructure,
    [switch]$CheckDomain,
    [string]$Scenario,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$RunnerArguments = @()
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $false
. (Join-Path $PSScriptRoot 'scripts/run-helpers.ps1')
Initialize-Console
$root = $PSScriptRoot
Set-Location $root
New-Item -ItemType Directory -Force (Join-Path $root '.logs') | Out-Null
if ($CheckStructure -and $CheckDomain) { throw 'Choose one domain or structure check.' }
if ($CheckStructure) { $RunnerArguments = @('--check-structure') + $RunnerArguments }
elseif ($CheckDomain) { $RunnerArguments = @('--check-domain') + $RunnerArguments }
elseif ($Scenario) { $RunnerArguments = @('--scenario', $Scenario) + $RunnerArguments }
$scenarioRun = -not ($RunnerArguments -contains '--check-structure' -or $RunnerArguments -contains '--check-domain')
if (-not ($Attach -or $Local -or $BuildOnly -or $NoBuild) -and $scenarioRun -and (Test-Path .logs/server.json)) {
    & node scripts/run-server.mjs --status
    $Attach = $LASTEXITCODE -eq 0
}
if ($Attach) {
    if ($BuildOnly -or -not $scenarioRun) { throw '-Attach requires a scenario run.' }
    & node scripts/run-server.mjs --attach @RunnerArguments
    exit $LASTEXITCODE
}
if ($Local) {
    if (-not $NoBuild) {
        & node scripts/run-server.mjs --build
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    if ($BuildOnly) { exit 0 }
    & node dist/e2e/runner.js @RunnerArguments
    exit $LASTEXITCODE
}
if ($NoBuild) { throw '-NoBuild requires -Local; Docker builds reuse the layer cache automatically.' }
if (-not $IsWindows -and (Test-Path '/var/run/docker.sock')) {
    $env:MY_GAME_RUNNER_USER = "$(& id -u):$(& id -g)"
    $env:MY_GAME_DOCKER_GID = & stat -c '%g' /var/run/docker.sock
    if ($LASTEXITCODE -ne 0) { $env:MY_GAME_DOCKER_GID = & stat -f '%g' /var/run/docker.sock }
}
$project = 'my-game-e2e-' + [guid]::NewGuid().ToString('N')
$logs = Join-Path $root ".logs/docker-$project"
New-Item -ItemType Directory $logs | Out-Null
# BuildKit reuses the previous transfer of a build context with the same directory name and
# compares only file size and modification time, so two projects named my-game with equal
# timestamps would build each other's files. A link with a per-run name gives every build its own
# transfer.
$env:MY_GAME_BUILD_CONTEXT = Join-Path $logs $project
New-Item -ItemType ($IsWindows ? 'Junction' : 'SymbolicLink') -Path $env:MY_GAME_BUILD_CONTEXT -Target $root | Out-Null
$compose = @('compose', '-f', (Join-Path $root 'docker-compose.yml'), '-p', $project, '--profile', 'docker')
$result = 1
Write-Host "Docker logs: $logs"
try {
    & docker @compose build *> (Join-Path $logs 'build.log')
    if ($LASTEXITCODE -ne 0) {
        Get-Content (Join-Path $logs 'build.log') -Tail 30 | Write-Host
        throw 'Docker image build failed.'
    }
    if ($BuildOnly) { $result = 0 }
    else {
        if ($CheckStructure) {
            & docker @compose run --rm --no-deps -T --name "$project-check" runner @RunnerArguments
            if ($LASTEXITCODE -ne 0) { throw 'Structure check failed.' }
            $result = 0
        }
        elseif ($CheckDomain) {
            & docker @compose run --rm --no-deps -T --name "$project-domain" runner @RunnerArguments
            if ($LASTEXITCODE -ne 0) { throw 'Domain check failed.' }
            & docker @compose run --rm --no-deps -T --name "$project-wire" node-image dist/e2e/wire-checks.js
            if ($LASTEXITCODE -ne 0) { throw 'Wire check failed.' }
            $result = 0
        }
        else {
            & docker @compose run --rm --no-deps -T --name "$project-check" runner --check-structure
            if ($LASTEXITCODE -ne 0) { throw 'Structure check failed.' }
            & docker @compose run --rm --no-deps -T --name "$project-domain" runner --check-domain
            if ($LASTEXITCODE -ne 0) { throw 'Domain check failed.' }
            & docker @compose run --rm --no-deps -T --name "$project-wire" node-image dist/e2e/wire-checks.js
            if ($LASTEXITCODE -ne 0) { throw 'Wire check failed.' }
            & docker @compose up -d --wait --wait-timeout 30 redis *> (Join-Path $logs 'redis-start.log')
            if ($LASTEXITCODE -ne 0) { throw 'Redis startup failed.' }
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
        & docker rm -f @containers *> (Join-Path $logs 'containers-stop.log')
        if ($LASTEXITCODE -ne 0) { $result = 1 }
    }
    & docker @compose logs --no-color redis *> (Join-Path $logs 'redis.log')
    if ($LASTEXITCODE -ne 0) { $result = 1 }
    & docker @compose down --volumes *> (Join-Path $logs 'cleanup.log')
    if ($LASTEXITCODE -ne 0) { $result = 1 }
    foreach ($image in @("$project-node", "$project-runner")) {
        & docker image inspect $image *> $null
        if ($LASTEXITCODE -eq 0) {
            & docker image rm $image *>> (Join-Path $logs 'cleanup.log')
            if ($LASTEXITCODE -ne 0) { $result = 1 }
        }
    }
    [IO.Directory]::Delete($env:MY_GAME_BUILD_CONTEXT)
}
exit $result

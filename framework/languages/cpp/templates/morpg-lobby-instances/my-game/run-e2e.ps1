param(
    [switch]$Local,
    [switch]$NoBuild,
    [switch]$BuildOnly,
    [string]$Scenario,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$RunnerArguments = @()
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 or later is required.' }
$root = $PSScriptRoot
New-Item -ItemType Directory -Force (Join-Path $root '.logs') | Out-Null
if ($Scenario) { $RunnerArguments = @('--scenario', $Scenario) + $RunnerArguments }
if ($Local) {
    if (-not $NoBuild) {
        Push-Location $root
        try {
            & cmake --preset default *> (Join-Path $root '.logs/build.log')
            if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed; see .logs/build.log.' }
            & cmake --build --preset default *>> (Join-Path $root '.logs/build.log')
            if ($LASTEXITCODE -ne 0) { throw 'CMake build failed; see .logs/build.log.' }
        } finally { Pop-Location }
    }
    if ($BuildOnly) { exit 0 }
    & (Join-Path $root 'build/bin/my-game-e2e.exe') --root $root @RunnerArguments
    exit $LASTEXITCODE
}
if ($NoBuild) { throw '-NoBuild requires -Local; Docker builds reuse the layer cache automatically.' }
$project = 'my-game-e2e-' + [guid]::NewGuid().ToString('N')
$logs = Join-Path $root ".logs/docker-$project"
New-Item -ItemType Directory $logs | Out-Null
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
        & docker @compose run --rm --no-deps -T --name "$project-check" runner @RunnerArguments --check-structure
        if ($LASTEXITCODE -ne 0) { throw 'Structure check failed.' }
        if ($RunnerArguments -contains '--check-structure') { $result = 0 }
        else {
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
}
exit $result

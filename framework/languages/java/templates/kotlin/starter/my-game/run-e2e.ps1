param(
    [switch]$Local,
    [switch]$Isolated,
    [switch]$CheckStructure,
    [switch]$NoBuild,
    [switch]$BuildOnly,
    [string]$Scenario,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$RunnerArguments = @()
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'scripts/run-helpers.ps1')
Initialize-Console
$root = $PSScriptRoot
Push-Location $root
try {
New-Item -ItemType Directory -Force (Join-Path $root '.logs') | Out-Null
if ($CheckStructure) { $RunnerArguments = @('--check-structure') + $RunnerArguments }
elseif ($Scenario) { $RunnerArguments = @('--scenario', $Scenario) + $RunnerArguments }
if (-not $BuildOnly -and -not $Isolated -and
    $RunnerArguments -notcontains '--check-structure' -and
    $RunnerArguments -notcontains '--check-domain' -and (Test-RunningServer)) {
    $java = Get-LocalJava
    & $java @javaFlags -cp "$e2eClasspath" MyGame.e2e.ProgramKt --attach @RunnerArguments
    exit $LASTEXITCODE
}
if ($Local) {
    if (-not $NoBuild) { Build-Local installDist }
    if ($BuildOnly) { exit 0 }
    $java = Get-LocalJava
    if (-not $java) { throw 'Build locally before using -NoBuild.' }
    & $java @javaFlags -cp "$e2eClasspath" MyGame.e2e.ProgramKt @RunnerArguments
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
        $checkArguments = @(if ($CheckStructure) { $RunnerArguments } else { '--check-structure' })
        & docker @compose run --rm --no-deps -T --name "$project-check" runner @checkArguments
        if ($LASTEXITCODE -ne 0) { throw 'Structure check failed.' }
        if ($CheckStructure) { $result = 0 }
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
    [IO.Directory]::Delete($env:MY_GAME_BUILD_CONTEXT)
}
exit $result

} finally { Pop-Location }

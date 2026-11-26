function Initialize-Console {
    if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 or later is required.' }
    [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
    $script:OutputEncoding = [Console]::OutputEncoding
}

function Get-Runner {
    $name = if ($IsWindows) { 'my-game-e2e.exe' } else { 'my-game-e2e' }
    Join-Path $root "build/bin/$name"
}

function Build-Local {
    $buildLog = Join-Path $root '.logs/build.log'
    New-Item -ItemType Directory -Force (Split-Path $buildLog) | Out-Null
    Push-Location $root
    try {
        Clear-Content $buildLog -ErrorAction SilentlyContinue
        foreach ($command in @(
            @{ Args = @('-P', 'bootstrap.cmake') }
            @{ Args = @('--preset', 'default') }
            @{ Args = @('--build', '--preset', 'default') }
        )) {
            $cmakeArguments = $command.Args
            & cmake @cmakeArguments *>> $buildLog
            if ($LASTEXITCODE -ne 0) {
                Get-Content $buildLog -Tail 20 | Write-Host
                throw 'Local build failed. See .logs/build.log.'
            }
        }
    } finally { Pop-Location }
}

function Test-RunningServer {
    $runner = Get-Runner
    if ((Test-Path (Join-Path $root '.logs/server.json')) -and (Test-Path $runner)) {
        & $runner --root $root --server-running
        return ($LASTEXITCODE -eq 0)
    }
    return $false
}

function Assert-NoRunningServer {
    if (Test-RunningServer) {
        throw 'A server is already running. Stop it with Ctrl+C in its terminal before building.'
    }
}

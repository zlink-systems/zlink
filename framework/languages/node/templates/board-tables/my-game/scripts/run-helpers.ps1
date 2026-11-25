function Initialize-Console {
    if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 or later is required.' }
    $utf8 = [Text.UTF8Encoding]::new($false)
    [Console]::OutputEncoding = $utf8
    $script:OutputEncoding = $utf8
}

function Test-ServerStatus([string]$Mode) {
    if (-not ((Test-Path '.logs/server.json') -and (Test-Path 'dist/e2e/runner.js'))) { return $false }
    & node dist/e2e/runner.js $Mode
    return ($LASTEXITCODE -eq 0)
}

function Assert-NoRunningServer {
    if (Test-ServerStatus '--server-active') {
        throw 'A server is already starting or running. Stop it with Ctrl+C before building.'
    }
}

function Build-Local {
    Write-Host 'Preparing: building the server (Node.js)...'
    & npm ci *> .logs/build.log
    if ($LASTEXITCODE -eq 0) { & npm run build *>> .logs/build.log }
    if ($LASTEXITCODE -ne 0) {
        Get-Content .logs/build.log -Tail 20 | Write-Host
        throw 'Node.js build failed.'
    }
    Write-Host 'Build complete. Starting servers...'
}

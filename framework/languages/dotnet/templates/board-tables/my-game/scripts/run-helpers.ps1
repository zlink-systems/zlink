if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 or later is required.' }
[Console]::InputEncoding = [Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Console]::OutputEncoding
$PSNativeCommandUseErrorActionPreference = $false
$runner = Join-Path $root 'e2e/MyGame.E2E/bin/Debug/net8.0/MyGame.E2E.dll'

function Test-RunningServer {
    if (!(Test-Path (Join-Path $root '.logs/server.json')) -or !(Test-Path $runner)) {
        return $false
    }
    & dotnet $runner --root $root --is-running | Out-Host
    return ($LASTEXITCODE -eq 0)
}

function Build-Local {
    if (Test-RunningServer) { throw 'Stop the running server with Ctrl+C before building locally.' }
    New-Item -ItemType Directory -Force (Join-Path $root '.logs') | Out-Null
    $log = Join-Path $root '.logs/build.log'
    & dotnet build (Join-Path $root 'server/MyGame.sln') -m:4 --nologo --verbosity minimal *> $log
    if ($LASTEXITCODE -ne 0) {
        Get-Content $log -Tail 20 | Write-Host
        throw "Server build failed. See $log."
    }
}

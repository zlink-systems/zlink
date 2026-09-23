param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('install', 'build', 'run', 'verify', 'stop')]
    [string]$Action
)

$ErrorActionPreference = 'Stop'
$RunDirectory = Join-Path $PSScriptRoot '.run'

function Find-FreePort([int]$Start, [int]$End) {
    for ($port = $Start; $port -le $End; $port++) {
        $listener = $null
        try {
            $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $port)
            $listener.Start()
            return $port
        } catch {
        } finally {
            if ($null -ne $listener) { $listener.Stop() }
        }
    }
    throw "no free port in $Start-$End"
}

function Stop-Run {
    $pidFile = Join-Path $RunDirectory 'server.pid'
    if (Test-Path -LiteralPath $pidFile) {
        $serverPid = (Get-Content -Raw $pidFile).Trim()
        if ($serverPid -match '^\d+$') {
            taskkill /PID $serverPid /T /F 2>$null | Out-Null
        }
    }
    $containerFile = Join-Path $RunDirectory 'redis.container'
    if (Test-Path -LiteralPath $containerFile) {
        $container = (Get-Content -Raw $containerFile).Trim()
        if ($container -match '^[0-9a-f]+$') {
            docker rm -fv $container 2>$null | Out-Null
        }
    }
    Remove-Item -Force -ErrorAction SilentlyContinue $pidFile, $containerFile
}

Push-Location $PSScriptRoot
try {
    switch ($Action) {
        'install' { dotnet restore EngineLobby.sln }
        'build' { dotnet build EngineLobby.sln -c Release }
        'run' {
            New-Item -ItemType Directory -Force -Path $RunDirectory | Out-Null
            $redisPort = Find-FreePort 22000 22099
            $meshPort = Find-FreePort 22100 22699
            $streamPort = Find-FreePort 22700 23299
            $httpPort = Find-FreePort 23300 23999
            $redisName = "zlink-redis-dotnet-engine-lobby-$PID-$([Guid]::NewGuid().ToString('N').Substring(0, 8))"

            $container = (docker create --name $redisName --tmpfs /data `
                -p "127.0.0.1:${redisPort}:6379" redis:7.2-alpine).Trim()
            $container | Out-File (Join-Path $RunDirectory 'redis.container') -NoNewline
            try {
                docker start $container | Out-Null
                $published = (docker inspect -f '{{(index (index .NetworkSettings.Ports "6379/tcp") 0).HostPort}}' $container).Trim()
                if ($published -ne $redisPort.ToString()) {
                    throw "Redis published port mismatch: expected $redisPort, got $published"
                }

                $redisReady = $false
                for ($index = 0; $index -lt 60; $index++) {
                    try {
                        $client = [Net.Sockets.TcpClient]::new('127.0.0.1', $redisPort)
                        $client.Dispose()
                        $redisReady = $true
                        break
                    } catch { Start-Sleep -Seconds 1 }
                }
                if (-not $redisReady) { throw 'Redis did not become ready' }

                $settings = @{
                    RedisEndpoint = "127.0.0.1:$redisPort"
                    RedisKeyPrefix = "engine-lobby-$([DateTimeOffset]::UtcNow.ToUnixTimeSeconds())-$PID"
                    MeshEndpoint = "tcp://127.0.0.1:$meshPort"
                    StreamEndpoint = "ws://127.0.0.1:$streamPort"
                    HttpEndpoint = "http://127.0.0.1:$httpPort"
                }
                $settings | ConvertTo-Json | Set-Content (Join-Path $RunDirectory 'settings.json')
                $streamPort | Out-File (Join-Path $RunDirectory 'stream.port') -NoNewline
                $httpPort | Out-File (Join-Path $RunDirectory 'http.port') -NoNewline

                $server = Start-Process dotnet -ArgumentList @(
                    'run', '--project', 'Server/EngineLobby.Server.csproj', '-c', 'Release', '--no-build',
                    '--', 'server', (Join-Path $RunDirectory 'settings.json')
                ) -RedirectStandardOutput (Join-Path $RunDirectory 'server.log') `
                  -RedirectStandardError (Join-Path $RunDirectory 'server.err.log') `
                  -PassThru -WindowStyle Hidden
                $server.Id | Out-File (Join-Path $RunDirectory 'server.pid') -NoNewline

                $ready = $false
                for ($index = 0; $index -lt 60; $index++) {
                    try {
                        $response = Invoke-RestMethod -Uri "http://127.0.0.1:$httpPort/ready"
                        if ($response.ready) { $ready = $true; break }
                    } catch { Start-Sleep -Seconds 1 }
                    if ($server.HasExited) { break }
                }
                if (-not $ready) { throw "Engine Lobby did not become ready; see .run/server.err.log" }
                Write-Output 'engine-lobby-server=ready'
            } catch {
                Stop-Run
                throw
            }
        }
        'verify' {
            $httpPort = (Get-Content -Raw (Join-Path $RunDirectory 'http.port')).Trim()
            $streamPort = (Get-Content -Raw (Join-Path $RunDirectory 'stream.port')).Trim()
            $response = Invoke-RestMethod -Uri "http://127.0.0.1:$httpPort/ready"
            if (-not $response.ready) { throw 'Engine Lobby readiness check failed' }
            dotnet run --project Server/EngineLobby.Server.csproj -c Release --no-build -- `
                probe "ws://127.0.0.1:$streamPort"
        }
        'stop' { Stop-Run }
    }
} finally {
    Pop-Location
}

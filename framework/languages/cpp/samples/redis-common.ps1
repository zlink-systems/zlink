Set-StrictMode -Version Latest

function Start-ZlinkSampleRedis {
    param(
        [Parameter(Mandatory = $true)][string]$Scope,
        [string]$Image = "redis:7.2-alpine"
    )
    $name = "$Scope-$PID-$([Guid]::NewGuid().ToString('N'))"
    $containerId = (& docker create --name $name --tmpfs /data -p "127.0.0.1::6379" $Image).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $containerId) {
        throw "Failed to create the dedicated Redis container: $name"
    }
    try {
        & docker start $containerId | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Failed to start the dedicated Redis container: $name" }
        $running = (& docker inspect -f '{{.State.Running}}' $containerId).Trim()
        $hostPort = (& docker inspect -f '{{(index (index .NetworkSettings.Ports "6379/tcp") 0).HostPort}}' $containerId).Trim()
        if ($LASTEXITCODE -ne 0 -or $running -ne "true" -or -not $hostPort) {
            throw "Failed to inspect the dedicated Redis container: $name"
        }
        return [PSCustomObject]@{ ContainerId = $containerId; Endpoint = "127.0.0.1:$hostPort" }
    } catch {
        & docker rm -fv $containerId 2>$null | Out-Null
        throw
    }
}

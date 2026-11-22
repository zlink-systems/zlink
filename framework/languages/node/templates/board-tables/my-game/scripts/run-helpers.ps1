# Shared by run-server.ps1 and run-e2e.ps1. Works on Windows PowerShell 5.1 and PowerShell 7.

function Assert-PowerShellVersion {
    if ($PSVersionTable.PSVersion -lt [version]'5.1') { throw 'PowerShell 5.1 or later is required.' }
}

# Windows PowerShell 5.1 has no $IsWindows and only runs on Windows.
function Test-OnWindows {
    return [System.Environment]::OSVersion.Platform -eq [System.PlatformID]::Win32NT
}

# Runs a native command and returns its exit code. Output goes to $Log as UTF-8, or is discarded
# when $Log is empty. Windows PowerShell 5.1 turns redirected native stderr into error records,
# which would stop the script under ErrorActionPreference=Stop, so this call judges the command by
# its exit code only.
function Invoke-Logged {
    param(
        [string]$Log,
        [switch]$Append,
        [Parameter(Mandatory = $true)][string]$Command,
        [string[]]$Arguments = @()
    )
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& $Command @Arguments 2>&1 | ForEach-Object { "$_" })
        $code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previous
    }
    if ($Log) {
        if ($Append) { Add-Content -LiteralPath $Log -Value $output -Encoding UTF8 }
        else { Set-Content -LiteralPath $Log -Value $output -Encoding UTF8 }
    }
    return $code
}

# True when the server recorded in logs/server.json still answers on its control port.
function Test-RunningServer {
    param([Parameter(Mandatory = $true)][string]$Root)
    $state = Join-Path $Root 'logs/server.json'
    if (-not (Test-Path -LiteralPath $state)) { return $false }
    try { $endpoint = [Uri](Get-Content -LiteralPath $state -Raw | ConvertFrom-Json).controlEndpoint }
    catch { return $false }
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $connect = $client.BeginConnect($endpoint.Host, $endpoint.Port, $null, $null)
        return ($connect.AsyncWaitHandle.WaitOne(1000) -and $client.Connected)
    }
    catch { return $false }
    finally { $client.Close() }
}

# npm ci replaces node_modules, which a running server still uses, so a build must not start then.
function Assert-NoRunningServer {
    param([Parameter(Mandatory = $true)][string]$Root)
    if (Test-RunningServer -Root $Root) {
        Write-Host 'A server is already running (logs/server.json). Stop it with Ctrl+C in its terminal first.' -ForegroundColor Red
        exit 1
    }
}

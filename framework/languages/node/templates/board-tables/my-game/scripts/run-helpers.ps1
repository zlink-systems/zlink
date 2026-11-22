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

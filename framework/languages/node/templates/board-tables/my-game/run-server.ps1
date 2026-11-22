$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 or later is required.' }
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force '.logs' | Out-Null
& npm ci *> '.logs/build.log'
if ($LASTEXITCODE -eq 0) {
    & npm run build *>> '.logs/build.log'
}
if ($LASTEXITCODE -ne 0) {
    Get-Content '.logs/build.log' -Tail 20 | Write-Host
    exit 1
}
& node dist/e2e/server-host.js
exit $LASTEXITCODE

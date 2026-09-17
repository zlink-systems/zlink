$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'redis-common.ps1')

# Regression for issue #537: the C++ ZoneWorld runner's Invoke-Child hardcoded
# "powershell.exe" under $PSHOME to relaunch its own isolated child lanes. That
# is only true on Windows PowerShell 5.1 (Desktop edition); pwsh 7 (Core
# edition) ships pwsh.exe there instead, so the child lane never started.
# Get-ZlinkSampleSelfShellPath (framework/languages/cpp/samples/redis-common.ps1)
# replaced the literal name with a resolution based on $PSVersionTable.PSEdition.

try {
    # Positive: the resolved path exists and genuinely behaves like a
    # PowerShell host when invoked the same way Invoke-Child invokes it.
    $resolved = Get-ZlinkSampleSelfShellPath
    if (-not (Test-Path -LiteralPath $resolved)) {
        throw "Resolved self-shell path does not exist: $resolved"
    }
    $probe = & $resolved -NoProfile -Command 'Write-Output ZLINK-SELF-SHELL-PROBE-OK'
    if (($probe -join "`n") -notmatch 'ZLINK-SELF-SHELL-PROBE-OK') {
        throw "Resolved self-shell path did not behave like a PowerShell host: $resolved"
    }

    # Negative control #1: a literal, single-name guess under $PSHOME is
    # exactly the bug this resolver fixes. Prove the guess for whichever
    # edition is NOT the current host is provably wrong on this machine, so
    # the control is not vacuous regardless of which shell runs this test.
    $otherEditionExeName = if ($PSVersionTable.PSEdition -eq 'Desktop') { 'pwsh.exe' } else { 'powershell.exe' }
    $wrongGuess = Join-Path $PSHOME $otherEditionExeName
    if (Test-Path -LiteralPath $wrongGuess) {
        throw "Test assumption violated: $otherEditionExeName unexpectedly exists under `$PSHOME ($PSHOME); revisit this negative control."
    }
    if ($resolved -eq $wrongGuess) {
        throw 'Resolver returned a hardcoded, wrong-edition executable name.'
    }
    Write-Host "Negative control #1 confirmed: hardcoding $otherEditionExeName under `$PSHOME ($PSHOME) is missing on this host, exactly as issue #537 reported for the mismatched edition."

    # Negative control #2: process introspection resolves to the OS-visible
    # image, which for pwsh installed as a dotnet global tool is dotnet.exe
    # hosting pwsh.dll rather than a directly relaunchable shell. Only
    # meaningful on that install shape; skip with a clear note otherwise.
    $viaProcessIntrospection = (Get-Process -Id $PID).Path
    $imageLeaf = Split-Path -Leaf $viaProcessIntrospection
    if ($PSVersionTable.PSEdition -eq 'Core' -and $imageLeaf -notin @('pwsh.exe', 'pwsh')) {
        if ($resolved -eq $viaProcessIntrospection) {
            throw "Resolver returned the non-shell process image ($viaProcessIntrospection) instead of a relaunchable pwsh executable."
        }
        Write-Host "Negative control #2 confirmed: (Get-Process -Id `$PID).Path resolves to $viaProcessIntrospection on this dotnet-tool pwsh install, which is not a relaunchable shell."
    } else {
        Write-Host "Negative control #2 skipped: this host's process image ($viaProcessIntrospection) is already a directly relaunchable shell, so it cannot demonstrate the dotnet-tool-install defect."
    }

    Write-Host "PASS cpp self-shell resolution: resolves a real, relaunchable PowerShell host on $($PSVersionTable.PSEdition) edition and rejects both a hardcoded executable name and a bare process-image path"
} catch {
    Write-Error $_
    exit 1
}

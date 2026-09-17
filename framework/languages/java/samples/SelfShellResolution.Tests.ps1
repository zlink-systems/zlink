$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'redis-common.ps1')

# Regression for issue #537 (Java and Kotlin share this file): both ZoneWorld
# runners' Invoke-IsolatedChild used (Get-Process -Id $PID).Path to find the
# executable to relaunch their own isolated crash/routing lanes. When pwsh is
# installed as a dotnet global tool, the OS-visible image for the running
# Core-edition process is dotnet.exe hosting the managed pwsh.dll, not a
# directly relaunchable pwsh.exe/pwsh shim, so the relaunch never started.
# Get-ZlinkSampleSelfShellPath (framework/languages/java/samples/redis-common.ps1)
# replaced the process introspection with a $PSHOME-based resolution.

try {
    # Positive: the resolved path exists and genuinely behaves like a
    # PowerShell host when invoked the same way Invoke-IsolatedChild invokes it.
    $resolved = Get-ZlinkSampleSelfShellPath
    if (-not (Test-Path -LiteralPath $resolved)) {
        throw "Resolved self-shell path does not exist: $resolved"
    }
    $probe = & $resolved -NoProfile -Command 'Write-Output ZLINK-SELF-SHELL-PROBE-OK'
    if (($probe -join "`n") -notmatch 'ZLINK-SELF-SHELL-PROBE-OK') {
        throw "Resolved self-shell path did not behave like a PowerShell host: $resolved"
    }

    # Negative control #1: a literal, single-name guess under $PSHOME would
    # reintroduce the sibling C++ defect (issue #537's reported form). Prove
    # the guess for whichever edition is NOT the current host is provably
    # wrong on this machine, so the control is not vacuous either way.
    $otherEditionExeName = if ($PSVersionTable.PSEdition -eq 'Desktop') { 'pwsh.exe' } else { 'powershell.exe' }
    $wrongGuess = Join-Path $PSHOME $otherEditionExeName
    if (Test-Path -LiteralPath $wrongGuess) {
        throw "Test assumption violated: $otherEditionExeName unexpectedly exists under `$PSHOME ($PSHOME); revisit this negative control."
    }
    if ($resolved -eq $wrongGuess) {
        throw 'Resolver returned a hardcoded, wrong-edition executable name.'
    }
    Write-Host "Negative control #1 confirmed: hardcoding $otherEditionExeName under `$PSHOME ($PSHOME) is missing on this host."

    # Negative control #2: this is the actual defect this file fixes. Process
    # introspection resolves to dotnet.exe on a dotnet-tool pwsh install,
    # which is not a relaunchable shell. Only meaningful on that install
    # shape; skip with a clear note otherwise.
    $viaProcessIntrospection = (Get-Process -Id $PID).Path
    $imageLeaf = Split-Path -Leaf $viaProcessIntrospection
    if ($PSVersionTable.PSEdition -eq 'Core' -and $imageLeaf -notin @('pwsh.exe', 'pwsh')) {
        if ($resolved -eq $viaProcessIntrospection) {
            throw "Resolver returned the non-shell process image ($viaProcessIntrospection) instead of a relaunchable pwsh executable."
        }
        Write-Host "Negative control #2 confirmed: (Get-Process -Id `$PID).Path resolves to $viaProcessIntrospection on this dotnet-tool pwsh install, which is not a relaunchable shell -- exactly the defect issue #537 traced for the Java/Kotlin runners."
    } else {
        Write-Host "Negative control #2 skipped: this host's process image ($viaProcessIntrospection) is already a directly relaunchable shell, so it cannot demonstrate the dotnet-tool-install defect."
    }

    Write-Host "PASS java/kotlin self-shell resolution: resolves a real, relaunchable PowerShell host on $($PSVersionTable.PSEdition) edition and rejects both a hardcoded executable name and the raw process-image path"
} catch {
    Write-Error $_
    exit 1
}

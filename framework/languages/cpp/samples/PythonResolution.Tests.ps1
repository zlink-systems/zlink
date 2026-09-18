$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'redis-common.ps1')

# Regression for issue #632 (the same defect PR #633 fixed for Java/Kotlin): the
# C++ ZoneWorld runner resolved the ZW-B8 fault proxy interpreter with
#     $python = Get-Command python.exe, python3.exe, py.exe -ErrorAction SilentlyContinue |
#         Select-Object -First 1
# which sees only PATH and trusts whatever it finds. On Windows that is wrong
# twice over: the python.org installer leaves "Add python.exe to PATH"
# unchecked by default, so an installed interpreter is invisible; and Windows 11
# puts App Execution Alias stubs named python.exe/python3.exe on PATH, which
# resolve and launch and then refuse to run anything (exit 9009) -- the C++
# runner would happily pick the stub and then fail at proxy start instead of
# falling back to its Node.js path, because the stub "resolves".
# Get-ZlinkSamplePythonCommand (framework/languages/cpp/samples/redis-common.ps1)
# replaced it with an ordered search -- PATH, py launcher, standard install
# roots -- where a candidate counts only after reporting a Python 3 version,
# and returns $null (not a throw) when nothing works, so the runner's existing
# Node.js fallback still applies.

# The defect as it shipped, kept verbatim so the negative controls exercise the
# real thing and not a paraphrase of it.
function Get-PythonTheOldWay {
    $python = Get-Command python.exe, python3.exe, py.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $python) { return $null }
    $arguments = if ($python.Name -eq "py.exe") { @("-3") } else { @() }
    return [pscustomobject]@{ Path = $python.Source; Arguments = $arguments }
}

# Runs a resolved interpreter the way the runner does: hand it a script and see
# whether the script actually executes.
function Test-InterpreterRunsScript {
    param([Parameter(Mandatory = $true)]$Command, [Parameter(Mandatory = $true)][string]$ScriptPath)

    if ($null -eq $Command) { return $false }
    try {
        $output = (& $Command.Path @(@($Command.Arguments) + @($ScriptPath)) 2>&1 | Out-String)
    } catch {
        return $false
    }
    return ($LASTEXITCODE -eq 0 -and $output -match 'ZLINK-PYTHON-PROBE-OK')
}

$originalPath = $env:PATH
$probeDir = Join-Path ([IO.Path]::GetTempPath()) "zlink-python-resolution-cpp-$PID"
New-Item -ItemType Directory -Force -Path $probeDir | Out-Null
$probeScript = Join-Path $probeDir 'probe.py'
Set-Content -LiteralPath $probeScript -Encoding utf8 -Value 'print("ZLINK-PYTHON-PROBE-OK")'

try {
    # Positive: on this machine, as configured, the resolver finds an
    # interpreter that really runs the fault proxy's kind of script.
    $resolved = Get-ZlinkSamplePythonCommand
    if (-not (Test-InterpreterRunsScript $resolved $probeScript)) {
        throw "Resolved interpreter did not run a Python script: $($resolved.Path) $($resolved.Arguments -join ' ')"
    }
    Write-Host "Positive: resolver returned a working interpreter -- $($resolved.Path) $($resolved.Arguments -join ' ')"

    # Negative control #1: the reported failure. Strip every directory that
    # holds a python/py executable from PATH, which is exactly the state of a
    # default python.org install, and show the old PATH-only lookup goes blind
    # (it would fall through to the Node.js proxy instead) while the new
    # resolver still finds the install.
    $strippedPath = (($originalPath -split [IO.Path]::PathSeparator) | Where-Object {
        $dir = $_
        if ([string]::IsNullOrWhiteSpace($dir)) { return $false }
        -not (@('python.exe', 'python3.exe', 'py.exe', 'python', 'python3', 'py') | Where-Object {
            Test-Path -LiteralPath (Join-Path $dir $_) -PathType Leaf
        })
    }) -join [IO.Path]::PathSeparator

    $env:PATH = $strippedPath
    $oldWay = Get-PythonTheOldWay
    if ($null -ne $oldWay) {
        throw "Test assumption violated: PATH stripping left $($oldWay.Path) reachable; revisit this negative control."
    }
    Write-Host 'Negative control #1 confirmed: with no Python on PATH the shipped lookup resolves nothing and the C++ runner would silently fall back to Node.js instead of using the installed Python.'
    if ($IsWindows) {
        # Recovering from an empty PATH is a Windows capability: it rests on the
        # py launcher and the standard install roots the installer writes to.
        $resolvedOffPath = Get-ZlinkSamplePythonCommand
        if (-not (Test-InterpreterRunsScript $resolvedOffPath $probeScript)) {
            throw "Resolver failed to find a working interpreter with Python off PATH: $($resolvedOffPath.Path)"
        }
        Write-Host "Negative control #1 fixed: off PATH, the resolver found $($resolvedOffPath.Path) $($resolvedOffPath.Arguments -join ' ')"
    } else {
        Write-Host 'Negative control #1 recovery skipped: off-PATH recovery uses the Windows py launcher and install roots, and this host is not Windows.'
    }
    $env:PATH = $originalPath

    # Negative control #2: the Windows App Execution Alias stub. It is an
    # executable named python.exe that satisfies Get-Command and then refuses
    # to run anything, so "found" and "works" are not the same predicate. The
    # old lookup would hand this straight to the runner, which would then fail
    # starting the proxy instead of ever trying the Node.js fallback.
    $aliasDir = if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps' } else { $null }
    $aliasStub = if ($aliasDir) { Join-Path $aliasDir 'python.exe' } else { $null }
    if ($aliasStub -and (Test-Path -LiteralPath $aliasStub -PathType Leaf)) {
        $env:PATH = ($aliasDir, $strippedPath) -join [IO.Path]::PathSeparator
        $oldWayStub = Get-PythonTheOldWay
        if ($null -eq $oldWayStub -or $oldWayStub.Path -ne $aliasStub) {
            throw "Test assumption violated: expected the shipped lookup to resolve $aliasStub, got $($oldWayStub.Path)."
        }
        if (Test-InterpreterRunsScript $oldWayStub $probeScript) {
            throw "Test assumption violated: $aliasStub ran a Python script, so it is not a Store alias stub; revisit this negative control."
        }
        Write-Host "Negative control #2 confirmed: the shipped lookup resolves the Store alias stub $aliasStub, which cannot run the fault proxy (exit $LASTEXITCODE)."
        $resolvedPastStub = Get-ZlinkSamplePythonCommand
        if ($resolvedPastStub.Path -eq $aliasStub) {
            throw 'Resolver returned the Store alias stub instead of a real interpreter.'
        }
        if (-not (Test-InterpreterRunsScript $resolvedPastStub $probeScript)) {
            throw "Resolver skipped the stub but did not return a working interpreter: $($resolvedPastStub.Path)"
        }
        Write-Host "Negative control #2 fixed: the resolver skipped the stub and returned $($resolvedPastStub.Path) $($resolvedPastStub.Arguments -join ' ')"
        $env:PATH = $originalPath
    } else {
        Write-Host 'Negative control #2 skipped: no Windows App Execution Alias stub on this host, so it cannot demonstrate the "resolves but refuses to run" shape.'
    }

    Write-Host 'PASS cpp ZW-B8 python resolution: finds a working Python 3 off PATH and rejects an executable that resolves but cannot run'
} catch {
    $env:PATH = $originalPath
    Write-Error $_
    exit 1
} finally {
    $env:PATH = $originalPath
    Remove-Item -LiteralPath $probeDir -Recurse -Force -ErrorAction SilentlyContinue
}

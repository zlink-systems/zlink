$ErrorActionPreference = "Stop"

$ScriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildScript = Join-Path $ScriptDirectory "..\build.ps1"
$Tokens = $null
$ParseErrors = $null
$Ast = [System.Management.Automation.Language.Parser]::ParseFile(
    $BuildScript,
    [ref]$Tokens,
    [ref]$ParseErrors
)

if ($ParseErrors.Count -ne 0) {
    throw "build.ps1 has PowerShell parse errors: $($ParseErrors.Message -join '; ')"
}

$FunctionAst = $Ast.Find(
    {
        param($Node)
        $Node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $Node.Name -eq "Invoke-CoreCtest"
    },
    $true
)
if ($null -eq $FunctionAst) {
    throw "Invoke-CoreCtest was not found in build.ps1."
}
Invoke-Expression $FunctionAst.Extent.Text

$BuildSource = [System.IO.File]::ReadAllText($BuildScript)
if ($BuildSource -match "Acceptable number of test failures" -or
    $BuildSource -match "tests\? failed" -or
    $BuildSource -match "failedCount") {
    throw "build.ps1 must not parse or allow a failed-test count."
}
if ($BuildSource -notmatch 'if \(\$Architecture -eq "arm64"\)[\s\S]*Skipping tests: Cannot run ARM64 binaries on x64 host') {
    throw "build.ps1 must retain the explicit ARM64 cross-build test skip."
}

$TestRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("zlink-ctest-verdict-" + [guid]::NewGuid().ToString("N"))
[System.IO.Directory]::CreateDirectory($TestRoot) | Out-Null
$OriginalPath = $env:PATH
$script:CtestExitCode = 0
$script:CtestInvocations = @()

function ctest {
    param(
        [Parameter(ValueFromRemainingArguments = $true)]
        [object[]]$Arguments
    )

    $script:CtestInvocations += ,@($Arguments)
    Write-Output "stub ctest output"
    $global:LASTEXITCODE = $script:CtestExitCode
}

try {
    Invoke-CoreCtest -BuildType "Release" -DllDirectory $TestRoot
    if ($script:CtestInvocations.Count -ne 1) {
        throw "Expected one ctest invocation, got $($script:CtestInvocations.Count)."
    }
    if (($script:CtestInvocations[0] -join " ") -ne "--output-on-failure -C Release --parallel") {
        throw "Unexpected ctest arguments: $($script:CtestInvocations[0] -join ' ')"
    }
    if ($env:PATH -ne $OriginalPath) {
        throw "PATH was not restored after successful ctest execution."
    }

    $script:CtestExitCode = 7
    $Failure = $null
    try {
        Invoke-CoreCtest -BuildType "Release" -DllDirectory $TestRoot
    } catch {
        $Failure = $_
    }
    if ($null -eq $Failure) {
        throw "A nonzero ctest exit code was accepted."
    }
    if ($Failure.Exception.Message -ne "ctest failed with exit code 7.") {
        throw "Unexpected ctest failure: $($Failure.Exception.Message)"
    }
    if ($env:PATH -ne $OriginalPath) {
        throw "PATH was not restored after failed ctest execution."
    }
} finally {
    $env:PATH = $OriginalPath
    Remove-Item -LiteralPath $TestRoot -Recurse -Force
}

Write-Host "PASS: build.ps1 propagates the ctest verdict and preserves the ARM64 skip."

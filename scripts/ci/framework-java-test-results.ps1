[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("summarize", "compare")]
    [string]$Mode,

    [Parameter(Mandatory = $true)]
    [string]$InputPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [string]$Platform = "",

    [int]$MinimumTests = 1
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-Utf8File([string]$Path, [string[]]$Lines) {
    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    [IO.File]::WriteAllLines(
        [IO.Path]::GetFullPath($Path),
        $Lines,
        [Text.UTF8Encoding]::new($false))
}

function Get-TestIdentity($TestCase) {
    $className = [string]$TestCase.classname
    $testName = [string]$TestCase.name
    if ([string]::IsNullOrWhiteSpace($className)) {
        return $testName
    }
    return "$className.$testName"
}

function Write-Summary {
    if ([string]::IsNullOrWhiteSpace($Platform)) {
        throw "-Platform is required in summarize mode"
    }
    if ($MinimumTests -lt 1) {
        throw "-MinimumTests must be at least 1"
    }

    $xmlFiles = @(Get-ChildItem -LiteralPath $InputPath -Filter "*.xml" -File -Recurse -ErrorAction SilentlyContinue)
    $testCount = 0
    $failureCount = 0
    $failedTests = [Collections.Generic.List[string]]::new()

    foreach ($xmlFile in $xmlFiles) {
        [xml]$document = Get-Content -LiteralPath $xmlFile.FullName -Raw
        $suite = $document.testsuite
        if ($null -eq $suite) {
            throw "JUnit XML does not have a testsuite root: $($xmlFile.FullName)"
        }
        $testCount += [int]$suite.GetAttribute("tests")
        $failureCount += [int]$suite.GetAttribute("failures") + [int]$suite.GetAttribute("errors")
        foreach ($testCase in @($suite.testcase)) {
            if ($null -ne $testCase.SelectSingleNode("failure | error")) {
                $failedTests.Add((Get-TestIdentity $testCase))
            }
        }
    }

    $failedTests = @($failedTests | Sort-Object -Unique)
    $summary = [ordered]@{
        platform = $Platform
        tests = $testCount
        failures = $failureCount
        failedTests = $failedTests
    }
    $summaryJson = ($summary | ConvertTo-Json -Depth 4) + [Environment]::NewLine
    Write-Utf8File -Path (Join-Path $OutputPath "summary.json") -Lines @($summaryJson)

    $report = @(
        "platform=$Platform",
        "tests=$testCount",
        "failures=$failureCount",
        "minimum_tests=$MinimumTests",
        "failed_tests:"
    ) + @($failedTests | ForEach-Object { "- $_" })
    Write-Utf8File -Path (Join-Path $OutputPath "results.txt") -Lines $report

    Write-Host "${Platform}: tests=$testCount failures=$failureCount"
    if ($testCount -lt $MinimumTests) {
        throw "$Platform executed $testCount tests; expected at least $MinimumTests"
    }
}

function Compare-Summaries {
    $summaryFiles = @(Get-ChildItem -LiteralPath $InputPath -Filter "summary.json" -File -Recurse -ErrorAction SilentlyContinue)
    $summaries = @{}
    foreach ($summaryFile in $summaryFiles) {
        $summary = Get-Content -LiteralPath $summaryFile.FullName -Raw | ConvertFrom-Json
        if ([string]::IsNullOrWhiteSpace([string]$summary.platform)) {
            throw "Test summary does not identify its platform: $($summaryFile.FullName)"
        }
        $summaries[[string]$summary.platform] = $summary
    }

    foreach ($required in @("linux-x64", "win-x64")) {
        if (-not $summaries.ContainsKey($required)) {
            Write-Utf8File -Path $OutputPath -Lines @(
                "comparison=unavailable",
                "missing_platform=$required",
                "windows_only_failures:"
            )
            throw "Missing Framework Java test summary for $required"
        }
    }

    $linux = $summaries["linux-x64"]
    $windows = $summaries["win-x64"]
    $linuxFailed = @($linux.failedTests | ForEach-Object { [string]$_ })
    $windowsFailed = @($windows.failedTests | ForEach-Object { [string]$_ })
    $windowsOnly = @($windowsFailed | Where-Object { $linuxFailed -notcontains $_ } | Sort-Object -Unique)
    $countsMatch = [int]$linux.failures -eq [int]$windows.failures

    $report = @(
        "comparison=$(if ($countsMatch) { 'match' } else { 'mismatch' })",
        "linux_tests=$($linux.tests)",
        "windows_tests=$($windows.tests)",
        "linux_failures=$($linux.failures)",
        "windows_failures=$($windows.failures)",
        "windows_only_failures:"
    ) + @($windowsOnly | ForEach-Object { "- $_" })
    Write-Utf8File -Path $OutputPath -Lines $report

    $report | ForEach-Object { Write-Host $_ }
    if (-not $countsMatch) {
        throw "Linux and Windows failure counts differ: $($linux.failures) != $($windows.failures)"
    }
}

switch ($Mode) {
    "summarize" { Write-Summary }
    "compare" { Compare-Summaries }
}

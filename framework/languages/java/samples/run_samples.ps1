[CmdletBinding()]
param(
    [string]$LocalPackageRoot = "",
    [string]$Sample = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RootDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. "$RootDir/redis-common.ps1"
if ($IsWindows) {
    Set-ZlinkSampleJavaRuntime -SamplesRoot $RootDir
}
$JavaRoot = Split-Path -Parent $RootDir
$ManifestPath = Join-Path $RootDir "sample-manifest.env"

if ($LocalPackageRoot) {
    $resolvedPackageRoot = [System.IO.Path]::GetFullPath($LocalPackageRoot)
    if (-not (Test-Path -LiteralPath (Join-Path $resolvedPackageRoot "maven") -PathType Container)) {
        throw "Local Maven package repository was not found: $resolvedPackageRoot/maven"
    }
    $env:ZLINK_LOCAL_PACKAGE_ROOT = $resolvedPackageRoot
}
if ($Sample) {
    $env:ZLINK_SAMPLE_FILTER = $Sample
}

Set-Location $RootDir

function Read-SampleManifest {
    param([string]$Path)
    $manifest = @{}
    Get-Content -Path $Path | ForEach-Object {
        $line = $_.Trim()
        if (-not $line -or $line.StartsWith("#")) {
            return
        }
        $parts = $line.Split("=", 2)
        if ($parts.Count -ne 2) {
            throw "Invalid sample manifest line: $line"
        }
        $value = $parts[1].Trim().Trim('"')
        $key = $parts[0].Trim()
        if ($key -eq "FORBIDDEN_SAMPLE_PATTERN") {
            $manifest[$key] = $value.Trim("'")
        } else {
            $manifest[$key] = @($value.Split(" ", [System.StringSplitOptions]::RemoveEmptyEntries))
        }
    }
    return $manifest
}

$Manifest = Read-SampleManifest $ManifestPath
$SampleFilter = if ($env:ZLINK_SAMPLE_FILTER) { $env:ZLINK_SAMPLE_FILTER } else { "" }
$PowerShell = Get-Command pwsh.exe, powershell.exe -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty Source
if (-not $PowerShell) {
    throw "PowerShell executable was not found."
}

function Invoke-Sample {
    param([string]$ScriptPath)
    if ($ScriptPath.EndsWith(".sh")) {
        & bash $ScriptPath
        if ($LASTEXITCODE -ne 0) {
            throw "Sample failed: $ScriptPath"
        }
        return
    }

    $process = Start-Process -FilePath $PowerShell `
        -ArgumentList @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$ScriptPath`"") `
        -WindowStyle Hidden -PassThru
    [void]$process.Handle
    try {
        $process.WaitForExit()
        $sampleExitCode = $process.ExitCode
    } finally {
        Stop-ZlinkSampleProcessTree -Process $process
        $process.Dispose()
    }

    if ($null -eq $sampleExitCode) {
        throw "Sample exit code was unavailable: $ScriptPath"
    }
    if ($sampleExitCode -ne 0) {
        throw "Sample failed: $ScriptPath"
    }
}

function Invoke-ManifestSamples {
    param([string]$Language, [string[]]$Samples)
    foreach ($sample in $Samples) {
        if ($SampleFilter -and $SampleFilter -ne $sample -and $SampleFilter -ne "$Language/$sample") {
            continue
        }
        $scriptName = if ($IsWindows) { "run_sample.ps1" } else { "run_sample.sh" }
        $script = Join-Path $RootDir "$Language/$sample/$scriptName"
        if (Test-Path $script) {
            Invoke-Sample $script
        } else {
            throw "sample runner missing: $Language/$sample ($script)"
        }
    }
}

$Gradle = if ($IsWindows) { Join-Path $JavaRoot "gradlew.bat" } else { Join-Path $JavaRoot "gradlew" }
Push-Location $JavaRoot
try {
    Invoke-ZlinkSampleGradleBuild -GradleExecutable $Gradle -Arguments @(
        "--no-daemon",
        ":zlink-framework-testkit:contractTest",
        "--tests",
        "*SampleReleaseGateContractTest*"
    )
    Invoke-ZlinkSampleGradleBuild -GradleExecutable $Gradle -Arguments @(
        "--no-daemon",
        "--no-parallel",
        ":zlink-framework-testkit:fakeBackendTest",
        "--tests",
        "systems.zlink.framework.testkit.CurrentManagerFakeBackendTest.actorAndSpotManagersExposeCurrentFluentCallsAgainstFakeBackend"
    )
} finally {
    Pop-Location
}
Write-Output "java fake-backend public-manager gate completed"

Invoke-ManifestSamples "java" $Manifest["JAVA_SAMPLES"]
Invoke-ManifestSamples "kotlin" $Manifest["KOTLIN_SAMPLES"]

Assert-ZlinkSampleSourcePolicy -Path $RootDir -Extension ".java", ".kt" `
    -Pattern $Manifest["FORBIDDEN_SAMPLE_PATTERN"] `
    -Message "sample gate failed: forbidden sample pattern found"

Write-Output "All Java/Kotlin samples passed"

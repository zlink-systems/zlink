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

    $stdoutLog = (New-TemporaryFile).FullName
    $stderrLog = (New-TemporaryFile).FullName
    $process = Start-Process -FilePath $PowerShell `
        -ArgumentList @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$ScriptPath`"") `
        -WindowStyle Hidden -RedirectStandardOutput $stdoutLog `
        -RedirectStandardError $stderrLog -Wait -PassThru

    $stdout = if (Test-Path -LiteralPath $stdoutLog) {
        Get-Content -Raw -LiteralPath $stdoutLog
    } else { "" }
    $stderr = if (Test-Path -LiteralPath $stderrLog) {
        Get-Content -Raw -LiteralPath $stderrLog
    } else { "" }
    if ($stdout) {
        Write-Output $stdout.TrimEnd()
    }
    if ($stderr) {
        [Console]::Error.WriteLine($stderr)
    }
    $sampleExitCode = $process.ExitCode
    $process.Dispose()
    if ($null -eq $sampleExitCode) {
        throw "Sample exit code was unavailable: $ScriptPath"
    }
    if ($sampleExitCode -ne 0) {
        [Console]::Error.WriteLine("sample stdout log: $stdoutLog")
        [Console]::Error.WriteLine("sample stderr log: $stderrLog")
        throw "Sample failed: $ScriptPath"
    }
    Remove-Item -LiteralPath $stdoutLog, $stderrLog -Force
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

$previousErrorActionPreference = $ErrorActionPreference
try {
    $ErrorActionPreference = "Continue"
    $offenders = & rg -n $Manifest["FORBIDDEN_SAMPLE_PATTERN"] $RootDir `
        -g "*.java" -g "*.kt" 2>&1
    $policyExitCode = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $previousErrorActionPreference
}
if ($policyExitCode -eq 0) {
    $offenders | ForEach-Object { [Console]::Error.WriteLine($_.ToString()) }
    throw "sample gate failed: forbidden sample pattern found"
}
if ($policyExitCode -ne 1) {
    throw "sample gate failed: source policy scan exited with code $policyExitCode"
}

Write-Output "All Java/Kotlin samples passed"

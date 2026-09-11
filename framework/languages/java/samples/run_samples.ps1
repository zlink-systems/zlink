[CmdletBinding()]
param(
    [string]$LocalPackageRoot = "",
    [string]$Sample = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RootDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue)) {
    $IsWindows = $env:OS -eq "Windows_NT"
}
$JavaRoot = Split-Path -Parent $RootDir
$RepositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $JavaRoot "../../.."))
. (Join-Path $JavaRoot "local-package-common.ps1")
$ManifestPath = Join-Path $RootDir "sample-manifest.env"

if ($IsWindows) {
    if (-not $LocalPackageRoot) {
        $LocalPackageRoot = Join-Path $RepositoryRoot ".artifacts/windows"
    }
    $resolvedPackageRoot = [System.IO.Path]::GetFullPath($LocalPackageRoot)
    $BindingVersion = Assert-ZlinkJavaLocalBindingPackage `
        -RepositoryRoot $RepositoryRoot `
        -LocalPackageRoot $resolvedPackageRoot
    $env:ZLINK_LOCAL_PACKAGE_ROOT = $resolvedPackageRoot
    $env:ZLINK_JAVA_REQUIRE_LOCAL_BINDING = "true"
    Write-Output "Using exact local Java binding package $BindingVersion from $resolvedPackageRoot/maven"
} elseif ($LocalPackageRoot) {
    $resolvedPackageRoot = [System.IO.Path]::GetFullPath($LocalPackageRoot)
    if (-not (Test-Path -LiteralPath (Join-Path $resolvedPackageRoot "maven") -PathType Container)) {
        throw "Local Maven package repository was not found: $resolvedPackageRoot/maven"
    }
    $env:ZLINK_LOCAL_PACKAGE_ROOT = $resolvedPackageRoot
}
. "$RootDir/redis-common.ps1"
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

function Invoke-SampleWithPortCollisionRetry {
    param([string]$ScriptPath)
    $output = New-TemporaryFile
    try {
        for ($attempt = 1; $attempt -le 3; $attempt++) {
            if ($IsWindows) {
                $previousErrorActionPreference = $ErrorActionPreference
                try {
                    # Windows PowerShell 5 promotes a native child's stderr to
                    # ErrorRecord. Preserve the combined log and judge the child
                    # strictly by its process exit code instead.
                    $ErrorActionPreference = "Continue"
                    & $PowerShell -NoProfile -ExecutionPolicy Bypass -File $ScriptPath *> $output
                    $exitCode = $LASTEXITCODE
                } finally {
                    $ErrorActionPreference = $previousErrorActionPreference
                }
            } else {
                & bash $ScriptPath *> $output
                $exitCode = $LASTEXITCODE
            }
            if ($exitCode -eq 0) {
                Get-Content $output
                return
            }
            $text = Get-Content $output -Raw
            $transientFailure = if ($IsWindows) {
                $text -match '(?i)(address already in use|port is already allocated|failed to bind host port|WSAEADDRINUSE|EADDRINUSE|Only one usage of each socket address)'
            } else {
                $text -match 'ZlinkBindException|BindException|Address already in use|EADDRINUSE|errno=98'
            }
            if (-not $transientFailure) {
                [Console]::Error.WriteLine($text)
                throw "Sample failed: $ScriptPath"
            }
            if ($attempt -eq 3) {
                [Console]::Error.WriteLine($text)
                throw "Sample failed after retries: $ScriptPath"
            }
            [Console]::Error.WriteLine(
                "sample transient port bind failure; retrying $ScriptPath ($attempt/3)")
        }
    } finally {
        Remove-Item -Force -ErrorAction SilentlyContinue $output
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
            Invoke-SampleWithPortCollisionRetry $script
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
        ":zlink-framework-core:test",
        "--tests",
        "systems.zlink.framework.runtime.actors.ZLinkActorRelocationStagingTest.entrySpotDestroyWaitsForActiveActorTurn"
    )
} finally {
    Pop-Location
}
Write-Output "java actor lifecycle sample gate completed"

Invoke-ManifestSamples "java" $Manifest["JAVA_SAMPLES"]
Invoke-ManifestSamples "kotlin" $Manifest["KOTLIN_SAMPLES"]

$sources = Get-ChildItem -Path $RootDir -Recurse -Include *.java,*.kt -File |
    Where-Object { $_.FullName -notmatch "[/\\](build|bin)[/\\]" }
$offenders = $sources | Select-String -Pattern $Manifest["FORBIDDEN_SAMPLE_PATTERN"]
if ($offenders) {
    $offenders | ForEach-Object { [Console]::Error.WriteLine($_.ToString()) }
    throw "sample gate failed: forbidden sample pattern found"
}

Write-Output "All Java/Kotlin samples passed"

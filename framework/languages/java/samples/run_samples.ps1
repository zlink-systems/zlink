Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RootDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$JavaRoot = Split-Path -Parent $RootDir
$CoreLib = Join-Path $JavaRoot "../../../core/build/lib/libzlink.so"
$ManifestPath = Join-Path $RootDir "sample-manifest.env"
if (Test-Path $CoreLib) {
    $env:ZLINK_LIBRARY_PATH = $CoreLib
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
function Invoke-Checked {
    param([string]$FilePath, [string[]]$Arguments)
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed: $FilePath $($Arguments -join ' ')"
    }
}

function Invoke-SampleWithRetry {
    param([string]$ScriptPath)
    $output = New-TemporaryFile
    try {
        for ($attempt = 1; $attempt -le 3; $attempt++) {
            if ($ScriptPath.EndsWith(".sh")) {
                & bash $ScriptPath *> $output
            } else {
                & pwsh -NoProfile -ExecutionPolicy Bypass -File $ScriptPath *> $output
            }
            if ($LASTEXITCODE -eq 0) {
                Get-Content $output
                return
            }
            $text = Get-Content $output -Raw
            if ($text -notmatch "ZlinkBindException|Timed out waiting") {
                Write-Error $text
                throw "Sample failed: $ScriptPath"
            }
            if ($attempt -eq 3) {
                Write-Error $text
                throw "Sample failed after retries: $ScriptPath"
            }
            Write-Error "sample transient port bind failure; retrying $ScriptPath ($attempt/3)"
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
            Invoke-SampleWithRetry $script
        } else {
            Write-Output "sample runner missing; skipped $Language/$sample"
        }
    }
}

$Gradle = if ($IsWindows) { Join-Path $JavaRoot "gradlew.bat" } else { Join-Path $JavaRoot "gradlew" }
Push-Location $JavaRoot
try {
    Invoke-Checked $Gradle @(
        "--no-daemon",
        ":zlink-framework-testkit:contractTest",
        "--tests",
        "*SampleReleaseGateContractTest*"
    )
    Invoke-Checked $Gradle @(
        "--no-daemon",
        "--no-parallel",
        ":zlink-framework-testkit:fakeBackendTest",
        "--tests",
        "systems.zlink.framework.testkit.ActorRuntimeFakeBackendTest.entrySpotDestroyActorRemovesEntryOwnedActorWithoutLeftCallback"
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
    $offenders | ForEach-Object { Write-Error $_.ToString() }
    throw "sample gate failed: forbidden sample pattern found"
}

Write-Output "All Java/Kotlin samples passed"

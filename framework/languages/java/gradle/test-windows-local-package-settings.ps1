[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($env:OS -ne "Windows_NT") {
    throw "This behavior test requires Windows."
}

$JavaRoot = Split-Path -Parent $PSScriptRoot
$RepositoryRoot = [IO.Path]::GetFullPath((Join-Path $JavaRoot "../../.."))
$Gradle = Join-Path $JavaRoot "gradlew.bat"
$originalRoot = $env:ZLINK_LOCAL_PACKAGE_ROOT
$originalStrict = $env:ZLINK_JAVA_REQUIRE_LOCAL_BINDING
$probeInit = New-TemporaryFile

function Invoke-SettingsProbe {
    param(
        [string]$LocalRoot,
        [string]$Strict,
        [string[]]$Properties = @(),
        [switch]$PrintRepository
    )

    $env:ZLINK_LOCAL_PACKAGE_ROOT = $LocalRoot
    $env:ZLINK_JAVA_REQUIRE_LOCAL_BINDING = $Strict
    $arguments = @(
        "--no-daemon", "--quiet", "help", "-Pzlink.includeSamples=false"
    ) + $Properties
    if ($PrintRepository) {
        $arguments += @("--init-script", $probeInit.FullName)
    }
    Push-Location $JavaRoot
    try {
        $previousErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $output = & $Gradle @arguments 2>&1 | Out-String
            $exitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousErrorActionPreference
        }
        return [PSCustomObject]@{ ExitCode = $exitCode; Output = $output }
    } finally {
        Pop-Location
    }
}

try {
    [IO.File]::WriteAllText($probeInit.FullName, @'
settingsEvaluated { settings ->
    def repository = settings.dependencyResolutionManagement.repositories
        .findByName('zlinkLocalPackages')
    println('ZLINK_LOCAL_MAVEN_REPOSITORY=' + repository.url)
}
'@, [Text.UTF8Encoding]::new($false))

    $rootA = Join-Path ([IO.Path]::GetTempPath()) "zlink-local-package-settings-a"
    $rootB = Join-Path ([IO.Path]::GetTempPath()) "zlink-local-package-settings-b"

    $accepted = Invoke-SettingsProbe -LocalRoot $rootA -Strict "true" `
        -Properties @(
            "-Pzlink.localPackageRoot=$rootA",
            "-Pzlink.requireLocalBinding=true") -PrintRepository
    if ($accepted.ExitCode -ne 0) {
        throw "Matching strict local-package settings failed:`n$($accepted.Output)"
    }
    if ($accepted.Output -notmatch [regex]::Escape(
            ([IO.Path]::GetFullPath((Join-Path $rootA "maven"))).Replace("\", "/"))) {
        throw "Gradle did not select the preflighted Windows Maven root:`n$($accepted.Output)"
    }

    $differentRoot = Invoke-SettingsProbe -LocalRoot $rootA -Strict "true" `
        -Properties @("-Pzlink.localPackageRoot=$rootB")
    if ($differentRoot.ExitCode -eq 0 -or
            $differentRoot.Output -notmatch "cannot override ZLINK_LOCAL_PACKAGE_ROOT") {
        throw "A conflicting Gradle local-package root was not rejected:`n$($differentRoot.Output)"
    }

    $disabledStrict = Invoke-SettingsProbe -LocalRoot $rootA -Strict "true" `
        -Properties @("-Pzlink.requireLocalBinding=false")
    if ($disabledStrict.ExitCode -eq 0 -or
            $disabledStrict.Output -notmatch "cannot disable Windows strict local-binding mode") {
        throw "A conflicting Gradle strict-mode property was not rejected:`n$($disabledStrict.Output)"
    }

    $canonical = Invoke-SettingsProbe -LocalRoot "" -Strict "" -PrintRepository
    if ($canonical.ExitCode -ne 0) {
        throw "Canonical Windows Maven selection failed:`n$($canonical.Output)"
    }
    if ($canonical.Output -notmatch "\.artifacts/windows/maven" -or
            $canonical.Output -match "\.artifacts/wsl/maven") {
        throw "Windows Gradle resolution did not select only the canonical Windows Maven path:`n$($canonical.Output)"
    }

    Write-Output "Windows local-package Gradle settings behavior: PASS"
} finally {
    $env:ZLINK_LOCAL_PACKAGE_ROOT = $originalRoot
    $env:ZLINK_JAVA_REQUIRE_LOCAL_BINDING = $originalStrict
    Remove-Item -LiteralPath $probeInit.FullName -Force -ErrorAction SilentlyContinue
}

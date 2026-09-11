Set-StrictMode -Version Latest

function Get-ZlinkJavaBindingVersion {
    param([Parameter(Mandatory = $true)][string]$RepositoryRoot)

    $versionFile = Join-Path $RepositoryRoot "bindings/java/VERSION"
    if (-not (Test-Path -LiteralPath $versionFile -PathType Leaf)) {
        throw "Java binding version file was not found: $versionFile"
    }
    $contents = (Get-Content -LiteralPath $versionFile -Raw).Trim()
    if ($contents -notmatch '^ZLINK_BINDING_VERSION=([0-9]+\.[0-9]+\.[0-9]+)$') {
        throw "Java binding version file must contain exactly ZLINK_BINDING_VERSION=X.Y.Z: $versionFile"
    }
    $bindingVersion = $Matches[1]

    $catalogFile = Join-Path $RepositoryRoot `
        "framework/languages/java/gradle/libs.versions.toml"
    $catalogContents = Get-Content -LiteralPath $catalogFile -Raw
    $catalogMatch = [regex]::Match(
        $catalogContents,
        '(?m)^zlinkBindings = "([0-9]+\.[0-9]+\.[0-9]+)"$')
    if (-not $catalogMatch.Success -or $catalogMatch.Groups[1].Value -ne $bindingVersion) {
        throw "Java Framework binding dependency must exactly match $versionFile in $catalogFile"
    }
    return $bindingVersion
}

function Assert-ZlinkJavaLocalBindingPackage {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][string]$LocalPackageRoot
    )

    $bindingVersion = Get-ZlinkJavaBindingVersion -RepositoryRoot $RepositoryRoot
    $artifactDirectory = Join-Path $LocalPackageRoot "maven/systems/zlink/zlink/$bindingVersion"
    $requiredFiles = @(
        "zlink-$bindingVersion.jar",
        "zlink-$bindingVersion.pom",
        "zlink-$bindingVersion.module"
    )
    $missingFiles = @($requiredFiles | Where-Object {
        $artifactPath = Join-Path $artifactDirectory $_
        -not (Test-Path -LiteralPath $artifactPath -PathType Leaf) -or
            (Get-Item -LiteralPath $artifactPath).Length -eq 0
    })
    if ($missingFiles.Count -ne 0) {
        throw "Exact Java binding package $bindingVersion is incomplete in $artifactDirectory. " +
            "Missing: $($missingFiles -join ', ')"
    }
    return $bindingVersion
}

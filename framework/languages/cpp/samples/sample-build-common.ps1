function Get-ZlinkCppSampleCacheValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildDir,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $CachePath = Join-Path $BuildDir "CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $CachePath)) {
        return $null
    }
    $Match = Select-String -LiteralPath $CachePath -Pattern "^$([regex]::Escape($Name)):[^=]*=(.*)$" |
        Select-Object -First 1
    if ($null -eq $Match) {
        return $null
    }
    return $Match.Matches[0].Groups[1].Value
}

function Test-ZlinkCppSampleBinaries {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BinDir,
        [Parameter(Mandatory = $true)]
        [string[]]$Names
    )

    foreach ($Name in $Names) {
        if (-not (Test-Path -LiteralPath (Join-Path $BinDir "$Name.exe") -PathType Leaf) -and
            -not (Test-Path -LiteralPath (Join-Path $BinDir $Name) -PathType Leaf)) {
            return $false
        }
    }
    return $true
}

function Resolve-ZlinkCppSampleBuild {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SampleDir,
        [Parameter(Mandatory = $true)]
        [string]$CppRoot,
        [Parameter(Mandatory = $true)]
        [string[]]$RequiredBinaries,
        [switch]$AllowMissingBinaries
    )

    # An explicit build directory is authoritative. Otherwise prefer this
    # sample's preset outputs before the shared Framework development build.
    $Roots = if ($env:ZLINK_CPP_BUILD_DIR) {
        @($env:ZLINK_CPP_BUILD_DIR)
    } else {
        @((Join-Path $SampleDir "build"), (Join-Path $CppRoot "build"))
    }
    $SearchRoots = @()
    foreach ($Root in $Roots) {
        $SearchRoots += [System.IO.Path]::GetFullPath($Root)
        if (Test-Path -LiteralPath $Root -PathType Container) {
            $SearchRoots += Get-ChildItem -LiteralPath $Root -Directory |
                Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "CMakeCache.txt") -PathType Leaf } |
                Sort-Object -Property @{ Expression = 'LastWriteTime'; Descending = $true }, Name |
                ForEach-Object { $_.FullName }
        }
    }

    $SearchRoots = @($SearchRoots | Select-Object -Unique)
    foreach ($BuildRoot in $SearchRoots) {
        $CachedConfiguration = Get-ZlinkCppSampleCacheValue -BuildDir $BuildRoot -Name "CMAKE_BUILD_TYPE"
        $Configurations = @()
        if ($env:ZLINK_CPP_BUILD_CONFIGURATION) {
            $Configurations += $env:ZLINK_CPP_BUILD_CONFIGURATION
        }
        if ($CachedConfiguration -in @("Debug", "Release", "RelWithDebInfo", "MinSizeRel")) {
            $Configurations += $CachedConfiguration
        }
        $Configurations += @("Debug", "Release", "RelWithDebInfo", "MinSizeRel")
        $Configurations = @($Configurations | Select-Object -Unique)
        $ExistingConfigurations = @($Configurations | Where-Object {
            Test-Path -LiteralPath (Join-Path $BuildRoot $_) -PathType Container
        })
        $CachedConfigurationIsAvailable =
            $CachedConfiguration -in @("Debug", "Release", "RelWithDebInfo", "MinSizeRel") -and
            (Test-Path -LiteralPath (Join-Path $BuildRoot $CachedConfiguration) -PathType Container)
        if (-not $env:ZLINK_CPP_BUILD_CONFIGURATION -and
            -not $CachedConfigurationIsAvailable -and
            $ExistingConfigurations.Count -eq 1) {
            $Configurations = @($ExistingConfigurations[0]) + @($Configurations | Where-Object { $_ -ne $ExistingConfigurations[0] })
        }

        $ConfigurationBinDirs = @($Configurations | ForEach-Object { Join-Path $BuildRoot $_ })
        $BinDirs = if ($AllowMissingBinaries -and $ExistingConfigurations.Count -gt 0) {
            $ConfigurationBinDirs + @($BuildRoot)
        } else {
            @($BuildRoot) + $ConfigurationBinDirs
        }
        foreach ($BinDir in $BinDirs) {
            $HasBinaries = Test-ZlinkCppSampleBinaries -BinDir $BinDir -Names $RequiredBinaries
            if (-not $HasBinaries -and -not $AllowMissingBinaries) { continue }
            if (-not $HasBinaries -and
                -not (Test-Path -LiteralPath (Join-Path $BuildRoot "CMakeCache.txt") -PathType Leaf)) {
                continue
            }
            $Configuration = $CachedConfiguration
            $BinLeaf = Split-Path -Leaf $BinDir
            if ($BinLeaf -in @("Debug", "Release", "RelWithDebInfo", "MinSizeRel")) {
                $Configuration = $BinLeaf
            } elseif ($env:ZLINK_CPP_BUILD_CONFIGURATION) {
                $Configuration = $env:ZLINK_CPP_BUILD_CONFIGURATION
            }
            if ($Configuration -notin @("Debug", "Release", "RelWithDebInfo", "MinSizeRel")) {
                $Configuration = "Debug"
            }
            return [pscustomobject]@{
                BuildDir = $BuildRoot
                BinDir = $BinDir
                Configuration = $Configuration
            }
        }
    }

    $Locations = if ($env:ZLINK_CPP_BUILD_DIR) {
        $env:ZLINK_CPP_BUILD_DIR
    } else {
        "$(Join-Path $SampleDir 'build') or $(Join-Path $CppRoot 'build')"
    }
    throw "Missing sample executables under $Locations. Build the sample preset or set ZLINK_CPP_BUILD_DIR."
}

function Get-ZlinkCppSampleBinary {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Build,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    foreach ($Path in @((Join-Path $Build.BinDir "$Name.exe"), (Join-Path $Build.BinDir $Name))) {
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            return $Path
        }
    }
    throw "Missing executable: $Name under $($Build.BinDir)"
}

function Invoke-ZlinkCppSampleCTest {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Build,
        [Parameter(Mandatory = $true)]
        [string]$CTestBin,
        [Parameter(Mandatory = $true)]
        [string]$Pattern
    )

    if (-not (Test-Path -LiteralPath (Join-Path $Build.BuildDir "CTestTestfile.cmake") -PathType Leaf)) {
        Write-Host "sample pre-run tests=skipped reason=no-ctest-metadata buildDir=$($Build.BuildDir)"
        return
    }

    $PreviousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        & $CTestBin --test-dir $Build.BuildDir -C $Build.Configuration -R $Pattern --output-on-failure
        $CTestExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $PreviousErrorActionPreference
    }
    if ($CTestExitCode -ne 0) {
        throw "$CTestBin failed with exit code $CTestExitCode"
    }
}

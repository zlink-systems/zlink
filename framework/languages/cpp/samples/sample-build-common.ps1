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
        if (-not (Test-Path -LiteralPath (Join-Path $BinDir "$Name.exe")) -and
            -not (Test-Path -LiteralPath (Join-Path $BinDir $Name))) {
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
        [string[]]$RequiredBinaries
    )

    $SearchRoots = @()
    if ($env:ZLINK_CPP_BUILD_DIR) {
        $SearchRoots += $env:ZLINK_CPP_BUILD_DIR
        if (Test-Path -LiteralPath $env:ZLINK_CPP_BUILD_DIR) {
            $SearchRoots += Get-ChildItem -LiteralPath $env:ZLINK_CPP_BUILD_DIR -Directory |
                Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "CMakeCache.txt") } |
                Sort-Object LastWriteTime -Descending |
                ForEach-Object { $_.FullName }
        }
    } else {
        foreach ($Root in @((Join-Path $SampleDir "build"), (Join-Path $CppRoot "build"))) {
            $SearchRoots += $Root
            if (Test-Path -LiteralPath $Root) {
                $SearchRoots += Get-ChildItem -LiteralPath $Root -Directory |
                    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "CMakeCache.txt") } |
                    Sort-Object LastWriteTime -Descending |
                    ForEach-Object { $_.FullName }
            }
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

        $BinDirs = @($BuildRoot)
        $BinDirs += $Configurations | ForEach-Object { Join-Path $BuildRoot $_ }
        foreach ($BinDir in $BinDirs) {
            if (-not (Test-ZlinkCppSampleBinaries -BinDir $BinDir -Names $RequiredBinaries)) {
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
        if (Test-Path -LiteralPath $Path) {
            return $Path
        }
    }
    throw "Missing executable: $Name under $($Build.BinDir)"
}

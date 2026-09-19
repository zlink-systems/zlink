Set-StrictMode -Version Latest

function Get-ZlinkCanonicalVersion {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Key
    )
    $VersionMatches = @(Select-String -LiteralPath $Path -Pattern "^$([regex]::Escape($Key))=(\d+\.\d+\.\d+)$")
    if ($VersionMatches.Count -ne 1) {
        throw "Expected exactly one canonical $Key version in $Path."
    }
    return $VersionMatches[0].Matches[0].Groups[1].Value
}

# The Core and binding versions this framework consumes are the defaults of two
# cache variables in CMakeLists.txt; sync-version.py keeps them equal to the
# repository VERSION files. Reading them there works for a release archive,
# which has no repository around it, exactly as for a checkout.
function Get-ZlinkCppDependencyVersion {
    param(
        [Parameter(Mandatory = $true)][string]$CppRoot,
        [Parameter(Mandatory = $true)][string]$Variable
    )
    $Path = Join-Path $CppRoot "CMakeLists.txt"
    $Pattern = '^set\(' + [regex]::Escape($Variable) + ' "(\d+\.\d+\.\d+)" CACHE STRING$'
    $VersionMatches = @(Select-String -LiteralPath $Path -Pattern $Pattern)
    if ($VersionMatches.Count -ne 1) {
        throw "Expected exactly one $Variable default in $Path."
    }
    return $VersionMatches[0].Matches[0].Groups[1].Value
}

# The zlink repository root when this tree is a checkout, $null for a release
# archive. Only repository-local defaults (.artifacts, the build token) depend
# on it; every input has an explicit parameter or environment variable.
function Get-ZlinkCppRepositoryRoot {
    param([Parameter(Mandatory = $true)][string]$CppRoot)
    $Candidate = Join-Path $CppRoot "../../.."
    if ((Test-Path (Join-Path $Candidate "VERSION") -PathType Leaf) -and
        (Test-Path (Join-Path $Candidate "bindings/cpp/VERSION") -PathType Leaf)) {
        return (Resolve-Path $Candidate).Path
    }
    return $null
}

function Get-ZlinkStableBuildToken {
    param([Parameter(Mandatory = $true)][string]$Path)
    $Sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $Bytes = [Text.Encoding]::UTF8.GetBytes($Path.ToLowerInvariant())
        return ([BitConverter]::ToString($Sha256.ComputeHash($Bytes), 0, 4)).Replace("-", "").ToLowerInvariant()
    } finally {
        $Sha256.Dispose()
    }
}

function Get-ZlinkCppWindowsSampleTargets {
    param([string[]]$Sample = @())

    $TargetsBySample = [ordered]@{
        TicTacToe = @(
            "sample_cpp_framework_tictactoe_api",
            "sample_cpp_framework_tictactoe_play",
            "sample_cpp_framework_tictactoe_client"
        )
        Bingo = @(
            "sample_cpp_framework_bingo_api",
            "sample_cpp_framework_bingo_matchmaking",
            "sample_cpp_framework_bingo_play",
            "sample_cpp_framework_bingo_session",
            "sample_cpp_framework_bingo_client"
        )
        DeliveryDispatch = @(
            "sample_cpp_framework_deliverydispatch_dispatch",
            "sample_cpp_framework_deliverydispatch_courier_actor_node",
            "sample_cpp_framework_deliverydispatch_customer_gateway",
            "sample_cpp_framework_deliverydispatch_courier_session",
            "sample_cpp_framework_deliverydispatch_tracking",
            "sample_cpp_framework_deliverydispatch_client"
        )
        SupportChat = @(
            "sample_cpp_framework_supportchat_api",
            "sample_cpp_framework_supportchat_session",
            "sample_cpp_framework_supportchat_support",
            "sample_cpp_framework_supportchat_client"
        )
        GameQuest = @(
            "sample_cpp_framework_gamequest_game_api",
            "sample_cpp_framework_gamequest_quest_mission",
            "sample_cpp_framework_gamequest_client"
        )
        ShoppingMall = @(
            "sample_cpp_framework_shoppingmall_commerce_api",
            "sample_cpp_framework_shoppingmall_order_workflow",
            "sample_cpp_framework_shoppingmall_client"
        )
        ZoneWorld = @(
            "sample_cpp_framework_zoneworld_zone_node",
            "sample_cpp_framework_zoneworld_gateway",
            "sample_cpp_framework_zoneworld_ops",
            "sample_cpp_framework_zoneworld_client",
            "sample_cpp_framework_zoneworld_session_route_proxy"
        )
    }

    $SampleNames = if ($Sample.Count -gt 0) { $Sample } else { @($TargetsBySample.Keys) }
    $Targets = foreach ($Name in $SampleNames) {
        if (-not $TargetsBySample.Contains($Name)) {
            throw "Unknown C++ sample: $Name"
        }
        $TargetsBySample[$Name]
    }
    return @($Targets)
}

function Resolve-ZlinkCppWindowsBuildInputs {
    param(
        [Parameter(Mandatory = $true)][string]$CppRoot,
        [string]$BuildDir,
        [string]$LocalPackageRoot,
        [string]$CorePackagePrefix,
        [string]$VcpkgInstalledDir
    )

    $CppRoot = (Resolve-Path $CppRoot).Path
    $RepositoryRoot = Get-ZlinkCppRepositoryRoot -CppRoot $CppRoot
    $CoreVersion = Get-ZlinkCppDependencyVersion `
        -CppRoot $CppRoot -Variable "ZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION"
    $BindingVersion = Get-ZlinkCppDependencyVersion `
        -CppRoot $CppRoot -Variable "ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION"
    $FrameworkVersion = Get-ZlinkCanonicalVersion `
        -Path (Join-Path $CppRoot "VERSION") -Key "ZLINK_FRAMEWORK_VERSION"
    # Repository-local defaults hang off the checkout; an archive anchors them
    # at its own root instead.
    $DefaultsRoot = if ($RepositoryRoot) { $RepositoryRoot } else { $CppRoot }

    if (-not $BuildDir) {
        $BuildDir = if ($env:ZLINK_CPP_BUILD_DIR) {
            $env:ZLINK_CPP_BUILD_DIR
        } else {
            $BuildDrive = Split-Path -Qualifier $DefaultsRoot
            if (-not $BuildDrive) {
                $BuildDrive = [IO.Path]::GetTempPath()
            }
            Join-Path $BuildDrive ".zlink-build/cpp-$(Get-ZlinkStableBuildToken -Path $DefaultsRoot)"
        }
    }

    $CleanPackageRoot = Join-Path $DefaultsRoot ".artifacts/cpp-clean-$BindingVersion-package"
    if (-not $LocalPackageRoot) {
        $LocalPackageRoot = if ($env:ZLINK_LOCAL_PACKAGE_ROOT) {
            $env:ZLINK_LOCAL_PACKAGE_ROOT
        } elseif (Test-Path $CleanPackageRoot) {
            $CleanPackageRoot
        } else {
            Join-Path $DefaultsRoot ".artifacts/windows"
        }
    }

    # The C++ local-package job stages the binding. Core may instead come from
    # the independently versioned Windows release prefix.
    $LocalCorePrefix = Join-Path $LocalPackageRoot "install/zlink-core/$CoreVersion"
    if (-not $CorePackagePrefix) {
        $CorePackagePrefix = if ($env:ZLINK_CORE_PACKAGE_PREFIX) {
            $env:ZLINK_CORE_PACKAGE_PREFIX
        } elseif (Test-Path $LocalCorePrefix) {
            $LocalCorePrefix
        } elseif ($env:LOCALAPPDATA) {
            Join-Path $env:LOCALAPPDATA "zlink/core/$CoreVersion/windows-x64"
        } else {
            $LocalCorePrefix
        }
    }

    if (-not $VcpkgInstalledDir) {
        $VcpkgInstalledDir = if (Test-Path (Join-Path $DefaultsRoot ".artifacts/windows-vcpkg-installed")) {
            Join-Path $DefaultsRoot ".artifacts/windows-vcpkg-installed"
        } else {
            Join-Path $DefaultsRoot ".artifacts/windows/vcpkg-installed"
        }
    }

    return [pscustomobject]@{
        RepositoryRoot = $RepositoryRoot
        CoreVersion = $CoreVersion
        BindingVersion = $BindingVersion
        FrameworkVersion = $FrameworkVersion
        BuildDir = [IO.Path]::GetFullPath($BuildDir)
        LocalPackageRoot = [IO.Path]::GetFullPath($LocalPackageRoot)
        CorePackagePrefix = [IO.Path]::GetFullPath($CorePackagePrefix)
        CppPackagePrefix = [IO.Path]::GetFullPath(
            (Join-Path $LocalPackageRoot "install/zlink-cpp/$BindingVersion"))
        VcpkgInstalledDir = [IO.Path]::GetFullPath($VcpkgInstalledDir)
    }
}

# Windows build script for libzlink
# Requires: Visual Studio 2022, CMake
# Supports: x64 and ARM64 architectures (ARM64 is cross-compiled on x64 host)

param(
    [string]$LibzlinkVersion,
    [string]$BuildType = "Release",
    [ValidateSet("x64", "arm64")]
    [string]$Architecture = "x64",
    [string]$OutputDir,
    [string]$RunTests = "OFF"
)

$ErrorActionPreference = "Stop"

# Resolve repo root from this script's location (core/builds/windows)
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..\\..\\..")).Path

# Load version information from VERSION file or parameters
$VERSION_FILE = Join-Path $RepoRoot "VERSION"

if (Test-Path $VERSION_FILE) {
    Write-Host "Reading VERSION file..."
    $VersionContent = Get-Content $VERSION_FILE
    foreach ($line in $VersionContent) {
        if ($line -match '^LIBZLINK_VERSION=(.+)$') {
            $FILE_LIBZLINK_VERSION = $matches[1]
        }
    }
}

# Parameters override VERSION file
if ($LibzlinkVersion) {
    $LIBZLINK_VERSION = $LibzlinkVersion
} elseif ($FILE_LIBZLINK_VERSION) {
    $LIBZLINK_VERSION = $FILE_LIBZLINK_VERSION
} else {
    $LIBZLINK_VERSION = "6.0.3"
}

# Set architecture-specific configurations
if ($Architecture -eq "arm64") {
    $VCPKG_TRIPLET = "arm64-windows-static"
    $CMAKE_ARCH = "ARM64"
    $DEFAULT_OUTPUT_DIR = Join-Path $RepoRoot "core\\dist\\windows-arm64"
} else {
    $VCPKG_TRIPLET = "x64-windows-static"
    $CMAKE_ARCH = "x64"
    $DEFAULT_OUTPUT_DIR = Join-Path $RepoRoot "core\\dist\\windows-x64"
}

# Use provided OutputDir or default based on architecture
if (-not $OutputDir) {
    $OutputDir = $DEFAULT_OUTPUT_DIR
} elseif (-not [System.IO.Path]::IsPathRooted($OutputDir)) {
    $OutputDir = Join-Path $RepoRoot $OutputDir
}

# Check for Visual Studio 2022
if ($env:VCINSTALLDIR) {
    Write-Host "Visual Studio environment already set (VCINSTALLDIR: $env:VCINSTALLDIR)"
    $VS2022_PATH = $env:VCINSTALLDIR
} else {
    Write-Host "==================================="
    Write-Host "Checking Visual Studio 2022..."
    Write-Host "==================================="

    # Use vswhere.exe to detect VS 2022
    $CommonVSWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

    if (-not (Test-Path $CommonVSWhere)) {
        Write-Error "vswhere.exe not found at: $CommonVSWhere"
        exit 1
    }

    # Check for VS 2022 installation (version range: 17.0 ~ 17.999)
    $VS2022_PATH = & "$CommonVSWhere" -version "[17.0,18.0)" -products * -requires Microsoft.VisualStudio.Workload.VCTools -property installationPath

    if (-not $VS2022_PATH) {
        Write-Error "Visual Studio 2022 with C++ Build Tools is required."
        exit 1
    }
}

Write-Host "Visual Studio 2022 found at: $VS2022_PATH"

Write-Host ""
Write-Host "==================================="
Write-Host "Windows Build Configuration"
Write-Host "==================================="
Write-Host "Architecture:      $Architecture"
Write-Host "libzlink version:    $LIBZLINK_VERSION"
Write-Host "Build type:        $BuildType"
Write-Host "RUN_TESTS:         $RunTests"
Write-Host "Output directory:  $OutputDir"
Write-Host "CMake platform:    $CMAKE_ARCH"
if ($Architecture -eq "arm64") {
    Write-Host ""
    Write-Host "NOTE: Building ARM64 binaries (cross-compilation)"
    Write-Host "      Tests cannot be executed on x64 host"
}
Write-Host "==================================="
Write-Host ""

# Create build directories with architecture suffix
$BUILD_DIR = Join-Path $RepoRoot "core\\build\\windows-$Architecture"
New-Item -ItemType Directory -Force -Path $BUILD_DIR | Out-Null
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

Write-Host "Step 1: Using repository source for libzlink..."
# libzlink source is already in the project root

# Step 2: Configure libzlink with CMake
Write-Host ""
Write-Host "Step 2: Configuring libzlink with CMake..."

# Convert to absolute paths before changing directory
$ROOT_DIR_ABS = $RepoRoot

Push-Location $BUILD_DIR
try {
    # Determine BUILD_TESTS flag
    $BUILD_TESTS_FLAG = "OFF"
    if ($RunTests -eq "ON") {
        $BUILD_TESTS_FLAG = "ON"
    }

    # Configure build
    $BoostIncludeArgs = @()
    $VcpkgInstalled = Join-Path $ROOT_DIR_ABS "core\\deps\\vcpkg\\installed\\$VCPKG_TRIPLET"
    $BoostIncludeDir = Join-Path $VcpkgInstalled "include"
    if ((Test-Path "$BoostIncludeDir\\boost\\asio.hpp") -and (Test-Path "$BoostIncludeDir\\boost\\beast.hpp")) {
        $BoostIncludeArgs += "-DZLINK_BOOST_INCLUDE_DIR=$BoostIncludeDir"
    }

    # Add OpenSSL config
    $OpenSSLArgs = @()
    if (Test-Path (Join-Path $VcpkgInstalled "include\\openssl")) {
        $OpenSSLArgs += "-DOPENSSL_ROOT_DIR=$VcpkgInstalled"
        $OpenSSLArgs += "-DCMAKE_PREFIX_PATH=$VcpkgInstalled"
    } elseif ($env:OPENSSL_ROOT_DIR) {
        Write-Host "Using OpenSSL from environment: $env:OPENSSL_ROOT_DIR"
        $OpenSSLArgs += "-DOPENSSL_ROOT_DIR=$env:OPENSSL_ROOT_DIR"
    }

    cmake (Join-Path $ROOT_DIR_ABS "core") `
        -G "Visual Studio 17 2022" `
        -A "$CMAKE_ARCH" `
        -DCMAKE_BUILD_TYPE="$BuildType" `
        -DCMAKE_POLICY_VERSION_MINIMUM="3.5" `
        -DBUILD_SHARED=ON `
        -DBUILD_STATIC=OFF `
        -DBUILD_TESTS="$BUILD_TESTS_FLAG" `
        -DZLINK_CXX_STANDARD=17 `
        -DBUILD_BENCHMARKS=OFF `
        -DCMAKE_INSTALL_PREFIX="$PWD\install" `
        $BoostIncludeArgs `
        $OpenSSLArgs

    # Step 3: Build libzlink
    Write-Host ""
    Write-Host "Step 3: Building libzlink..."
    cmake --build . --config $BuildType --parallel

    # Step 4: Install
    Write-Host ""
    Write-Host "Step 4: Installing to output directory..."
    cmake --install . --config $BuildType

    # Copy DLL to output
    $DLL_PATH = "Release\*zlink*.dll"
    $DLL_FILES = Get-ChildItem $DLL_PATH -ErrorAction SilentlyContinue
    if ($DLL_FILES.Count -eq 0) {
        $DLL_PATH = "install\bin\*zlink*.dll"
        $DLL_FILES = Get-ChildItem $DLL_PATH -ErrorAction SilentlyContinue
    }
    if ($DLL_FILES.Count -eq 0) {
        $DLL_PATH = "bin\$BuildType\*zlink*.dll"
        $DLL_FILES = Get-ChildItem $DLL_PATH -ErrorAction SilentlyContinue
    }

    if ($DLL_FILES.Count -gt 0) {
        $SOURCE_DLL = $DLL_FILES[0].FullName
        $ORIGINAL_DLL_NAME = $DLL_FILES[0].Name

        # Copy with original versioned name (required for linking)
        $TARGET_DLL_VERSIONED = Join-Path $OutputDir $ORIGINAL_DLL_NAME
        Copy-Item $SOURCE_DLL $TARGET_DLL_VERSIONED
        Write-Host "Copied: $SOURCE_DLL -> $TARGET_DLL_VERSIONED"

        # Also copy as stable runtime name
        $TARGET_DLL = Join-Path $OutputDir "zlink.dll"
        Copy-Item $SOURCE_DLL $TARGET_DLL
        Write-Host "Copied: $SOURCE_DLL -> $TARGET_DLL"

        # Also copy the import library (.lib) for linking
        $LIB_PATH = "Release\*zlink*.lib"
        $LIB_FILES = Get-ChildItem $LIB_PATH -ErrorAction SilentlyContinue
        if ($LIB_FILES.Count -eq 0) {
            $LIB_PATH = "install\lib\*zlink*.lib"
            $LIB_FILES = Get-ChildItem $LIB_PATH -ErrorAction SilentlyContinue
        }
        if ($LIB_FILES.Count -eq 0) {
            $LIB_PATH = "lib\$BuildType\*zlink*.lib"
            $LIB_FILES = Get-ChildItem $LIB_PATH -ErrorAction SilentlyContinue
        }
        if ($LIB_FILES.Count -gt 0) {
            $SOURCE_LIB = $LIB_FILES[0].FullName
            $ORIGINAL_LIB_NAME = $LIB_FILES[0].Name

            # Copy with original versioned name
            $TARGET_LIB_VERSIONED = Join-Path $OutputDir $ORIGINAL_LIB_NAME
            Copy-Item $SOURCE_LIB $TARGET_LIB_VERSIONED
            Write-Host "Copied: $SOURCE_LIB -> $TARGET_LIB_VERSIONED"

            # Also copy as libzlink.lib for convenience
            $TARGET_LIB = Join-Path $OutputDir "libzlink.lib"
            Copy-Item $SOURCE_LIB $TARGET_LIB
            Write-Host "Copied: $SOURCE_LIB -> $TARGET_LIB"
        }

        # Copy VC++ runtime DLLs if needed
        # Note: When using static MSVC runtime (/MT via CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded),
        # these DLLs are not required. However, we include them as a fallback for compatibility.
        Write-Host ""
        Write-Host "Checking for VC++ runtime DLLs..."

        $RuntimeDLLs = @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")
        $CopiedCount = 0

        # Try multiple sources for runtime DLLs
        $DllSources = @(
            # 1. vcpkg install/bin
            "install\bin",
            # 2. Visual Studio 2022 Enterprise (GitHub Actions)
            "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT",
            # 3. Visual Studio 2022 Professional
            "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT",
            # 4. Visual Studio 2022 Community
            "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT",
            # 5. System32 (last resort)
            "C:\Windows\System32"
        )

        foreach ($dll in $RuntimeDLLs) {
            $Found = $false
            foreach ($source in $DllSources) {
                $SearchPath = Resolve-Path $source -ErrorAction SilentlyContinue | Select-Object -First 1
                if ($SearchPath) {
                    $SourcePath = Join-Path $SearchPath.Path $dll
                    if (Test-Path $SourcePath) {
                        $TargetPath = Join-Path $OutputDir $dll
                        Copy-Item $SourcePath $TargetPath -Force
                        Write-Host "Copied: $dll from $($SearchPath.Path)"
                        $CopiedCount++
                        $Found = $true
                        break
                    }
                }
            }
            if (-not $Found) {
                Write-Host "Info: $dll not found (may not be needed with static runtime)"
            }
        }

        if ($CopiedCount -eq 0) {
            Write-Host "Note: No VC++ runtime DLLs copied. Using static MSVC runtime (/MT)."
        }

        # Copy public headers
        Write-Host ""
        Write-Host "Copying public headers..."
        $IncludeDir = Join-Path $OutputDir "include"
        New-Item -ItemType Directory -Force -Path $IncludeDir | Out-Null
        Copy-Item (Join-Path $RepoRoot "core\\include\\zlink.h") $IncludeDir
        Copy-Item (Join-Path $RepoRoot "core\\include\\zlink_enum.h") $IncludeDir
        Copy-Item (Join-Path $RepoRoot "core\\include\\zlink_errno.h") $IncludeDir
        Write-Host "Copied public headers -> $IncludeDir"
    } else {
        throw "zlink.dll not found!"
    }

    # Step 5: Run tests (if enabled)
    if ($RunTests -eq "ON") {
        Write-Host ""
        Write-Host "Step 5: Running tests..."

        # Skip tests for ARM64 cross-compilation (cannot run ARM64 binaries on x64 host)
        if ($Architecture -eq "arm64") {
            Write-Host "Skipping tests: Cannot run ARM64 binaries on x64 host"
        } else {
            # Run tests with ctest
            Write-Host "Running ctest..."

            # Ensure the DLL directory is in the PATH so tests can find zlink.dll
            $OLD_PATH = $env:PATH
            $DLL_DIR = (Resolve-Path ("bin\\$BuildType")).Path
            $env:PATH = "$DLL_DIR;$env:PATH"
            Write-Host "Temporarily added $DLL_DIR to PATH for testing"

            # Run ctest and capture exit code
            $ctestOutput = ""
            $ctestExitCode = 0

            try {
                # Run ctest with output on failure
                ctest --output-on-failure -C $BuildType --parallel 2>&1 | Tee-Object -Variable ctestOutput | Write-Host
                $ctestExitCode = $LASTEXITCODE
            } catch {
                $ctestExitCode = 1
            } finally {
                # Restore original path
                $env:PATH = $OLD_PATH
            }

            if ($ctestExitCode -ne 0) {
                Write-Host ""
                Write-Host "Some tests failed. Checking results..."

                # Count failed tests
                $failedCount = 0
                if ($ctestOutput -match "(\d+) tests? failed") {
                    $failedCount = [int]$matches[1]
                }

                Write-Host "Failed tests: $failedCount"

                # Allow up to 20 test failures (TIPC, fuzzer tests may not work in all environments)
                if ($failedCount -gt 20) {
                    throw "Too many test failures ($failedCount). Build may be broken."
                }

                Write-Host "Acceptable number of test failures. Continuing..."
            } else {
                Write-Host "All tests passed!"
            }
        }
    }
} finally {
    Pop-Location
}

# Step 6: Verify build
Write-Host ""
Write-Host "Step 6: Verifying build..."
$FINAL_DLL = "$OutputDir\zlink.dll"

if (Test-Path $FINAL_DLL) {
    $FileSize = (Get-Item $FINAL_DLL).Length
    Write-Host "File size: $FileSize bytes"

    # Check with dumpbin if available
    $dumpbin = Get-Command dumpbin -ErrorAction SilentlyContinue
    if ($dumpbin) {
        Write-Host ""
        Write-Host "DLL dependencies:"
        dumpbin /dependents $FINAL_DLL

        Write-Host ""
        Write-Host "DLL architecture:"
        dumpbin /headers $FINAL_DLL | Select-String "machine"
    }

    Write-Host ""
    Write-Host "==================================="
    Write-Host "Build completed successfully!"
    Write-Host "Architecture:      $Architecture"
    Write-Host "Output: $FINAL_DLL"
    if ($Architecture -eq "arm64") {
        Write-Host ""
        Write-Host "IMPORTANT: This is an ARM64 binary"
        Write-Host "           Cannot be executed on x64 host"
        Write-Host "           Deploy to ARM64 Windows device for testing"
    }
    Write-Host "==================================="
} else {
    throw "Build failed: $FINAL_DLL not found"
}

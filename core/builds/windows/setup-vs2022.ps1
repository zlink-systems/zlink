# Visual Studio 2022 Build Tools Setup Script
# This script installs Visual Studio 2022 Build Tools with components required for zlink builds
#
# Usage: .\setup-vs2022.ps1
#
# Components installed:
#   - Desktop development with C++
#   - C++ x64/x86 build tools
#   - C++ ARM64 build tools
#   - Windows 11 SDK (22621)
#   - CMake tools for Windows
#
# Note: This matches GitHub Actions windows-2022 runner configuration

param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"

# Check if running as Administrator
$IsAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $IsAdmin) {
    Write-Error ""
    Write-Error "=========================================="
    Write-Error "Administrator privileges required"
    Write-Error "=========================================="
    Write-Error "This script must be run as Administrator."
    Write-Error ""
    Write-Error "Please:"
    Write-Error "  1. Right-click PowerShell"
    Write-Error "  2. Select 'Run as Administrator'"
    Write-Error "  3. Run this script again"
    Write-Error "=========================================="
    exit 1
}

Write-Host "==================================="
Write-Host "Visual Studio 2022 Setup"
Write-Host "==================================="
Write-Host ""

# Check for existing VS 2022 installation
$CommonVSWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

if (Test-Path $CommonVSWhere) {
    Write-Host "Checking for existing Visual Studio 2022 installation..."
    $VS2022_PATH = & "$CommonVSWhere" -version "[17.0,18.0)" -products * -requires Microsoft.VisualStudio.Workload.VCTools -property installationPath

    if ($VS2022_PATH -and -not $Force) {
        Write-Host ""
        Write-Host "=========================================="
        Write-Host "Visual Studio 2022 already installed"
        Write-Host "=========================================="
        Write-Host "Installation path: $VS2022_PATH"
        Write-Host ""
        Write-Host "If you want to reinstall or modify components,"
        Write-Host "run with -Force flag:"
        Write-Host "  .\setup-vs2022.ps1 -Force"
        Write-Host "=========================================="
        exit 0
    } elseif ($VS2022_PATH -and $Force) {
        Write-Host "Existing installation found at: $VS2022_PATH"
        Write-Host "Force flag specified - will modify installation"
        Write-Host ""
    }
}

# Download VS Build Tools 2022 installer
$VS_BUILDTOOLS_URL = "https://aka.ms/vs/17/release/vs_BuildTools.exe"
$VS_INSTALLER = "$env:TEMP\vs_BuildTools.exe"

Write-Host "Downloading Visual Studio Build Tools 2022..."
Write-Host "URL: $VS_BUILDTOOLS_URL"
Write-Host ""

try {
    Invoke-WebRequest -Uri $VS_BUILDTOOLS_URL -OutFile $VS_INSTALLER -UseBasicParsing
    Write-Host "Download completed: $VS_INSTALLER"
} catch {
    Write-Error "Failed to download Visual Studio installer: $_"
    exit 1
}

# Prepare installation arguments
# These components match GitHub Actions windows-2022 runner configuration
Write-Host ""
Write-Host "=========================================="
Write-Host "Installing Components"
Write-Host "=========================================="
Write-Host "The following components will be installed:"
Write-Host ""
Write-Host "Workloads:"
Write-Host "  - Desktop development with C++"
Write-Host ""
Write-Host "Individual Components:"
Write-Host "  - MSVC v143 x64/x86 build tools"
Write-Host "  - MSVC v143 ARM64 build tools"
Write-Host "  - Windows 11 SDK (10.0.22621.0)"
Write-Host "  - CMake tools for Windows"
Write-Host ""
Write-Host "Installation may take 10-30 minutes depending on your internet speed."
Write-Host "Please do not close this window or interrupt the installation."
Write-Host "=========================================="
Write-Host ""

$InstallArgs = @(
    "--quiet",
    "--wait",
    "--norestart",
    "--add", "Microsoft.VisualStudio.Workload.VCTools",
    "--add", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
    "--add", "Microsoft.VisualStudio.Component.VC.Tools.ARM64",
    "--add", "Microsoft.VisualStudio.Component.Windows11SDK.22621",
    "--add", "Microsoft.VisualStudio.Component.VC.CMake.Project"
)

Write-Host "Starting installation..."
Write-Host ""

$InstallProcess = Start-Process -FilePath $VS_INSTALLER -ArgumentList $InstallArgs -Wait -PassThru -NoNewWindow

# Check installation result
if ($InstallProcess.ExitCode -eq 0) {
    Write-Host ""
    Write-Host "=========================================="
    Write-Host "Installation completed successfully!"
    Write-Host "=========================================="
    Write-Host ""

    # Verify installation
    if (Test-Path $CommonVSWhere) {
        $VS2022_PATH = & "$CommonVSWhere" -version "[17.0,18.0)" -products * -requires Microsoft.VisualStudio.Workload.VCTools -property installationPath

        if ($VS2022_PATH) {
            Write-Host "Visual Studio 2022 verified at:"
            Write-Host "  $VS2022_PATH"
            Write-Host ""
            Write-Host "You can now run the build script:"
            Write-Host "  .\core\builds\windows\build.ps1 -Architecture x64 -RunTests ON"
            Write-Host ""
        } else {
            Write-Warning "Installation completed but VS path verification failed."
            Write-Warning "You may need to restart your computer."
        }
    }
} elseif ($InstallProcess.ExitCode -eq 3010) {
    Write-Host ""
    Write-Host "=========================================="
    Write-Host "Installation completed - Restart required"
    Write-Host "=========================================="
    Write-Host ""
    Write-Host "Visual Studio Build Tools 2022 has been installed successfully."
    Write-Host "However, a system restart is required to complete the installation."
    Write-Host ""
    Write-Host "Please:"
    Write-Host "  1. Restart your computer"
    Write-Host "  2. Run the build script:"
    Write-Host "     .\core\builds\windows\build.ps1 -Architecture x64 -RunTests ON"
    Write-Host ""
    Write-Host "=========================================="
    exit 3010
} else {
    Write-Error ""
    Write-Error "=========================================="
    Write-Error "Installation failed"
    Write-Error "=========================================="
    Write-Error "Exit code: $($InstallProcess.ExitCode)"
    Write-Error ""
    Write-Error "Common solutions:"
    Write-Error "  1. Check internet connection"
    Write-Error "  2. Ensure sufficient disk space (10+ GB)"
    Write-Error "  3. Temporarily disable antivirus"
    Write-Error "  4. Try manual installation:"
    Write-Error "     https://aka.ms/vs/17/release/vs_BuildTools.exe"
    Write-Error "=========================================="
    exit $InstallProcess.ExitCode
}

# Clean up installer
Write-Host ""
Write-Host "Cleaning up installer..."
Remove-Item $VS_INSTALLER -ErrorAction SilentlyContinue
Write-Host "Done!"
Write-Host ""
Write-Host "=========================================="

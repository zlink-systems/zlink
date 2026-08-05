# Windows ARM64 Cross-Compilation Guide

This document explains how to build ARM64 binaries of libzlink on Windows x64 machines.

## Prerequisites

- Windows 10/11 x64 machine
- Visual Studio 2019 or later with ARM64 build tools installed
- CMake 3.5 or later
- Git
- PowerShell

### Installing ARM64 Build Tools

1. Open Visual Studio Installer
2. Click "Modify" on your Visual Studio installation
3. Go to "Individual components" tab
4. Search for and select:
   - "MSVC v143 - VS 2022 C++ ARM64 build tools" (or equivalent for your VS version)
   - "C++ ATL for latest v143 build tools (ARM64)" (optional)
5. Click "Modify" to install

## Usage

### Building x64 (default)

```powershell
.\core\builds\windows\build.ps1
```

Or explicitly:

```powershell
.\core\builds\windows\build.ps1 -Architecture x64
```

### Building ARM64 (cross-compilation)

```powershell
.\core\builds\windows\build.ps1 -Architecture arm64
```

### Full Example with All Parameters

```powershell
.\core\builds\windows\build.ps1 `
    -Architecture arm64 `
    -LibzlinkVersion "6.0.3" `
    -BuildType Release `
    -OutputDir "core\dist\windows-arm64"
```

## Architecture-Specific Configurations

| Parameter | x64 | arm64 |
|-----------|-----|-------|
| vcpkg triplet | `x64-windows-static` | `arm64-windows-static` |
| CMake platform | `-A x64` | `-A ARM64` |
| Build directory | `core/build/windows-x64` | `core/build/windows-arm64` |
| Deps directory | `deps/windows-x64` | `deps/windows-arm64` |
| Default output | `core/dist/windows-x64` | `core/dist/windows-arm64` |

**Note:** vcpkg itself is shared across architectures at `deps/vcpkg` to save disk space and clone time.

## Output Files

After a successful build, you'll find these files in the output directory:

- `zlink.dll` - Main Zlink library (ARM64 or x64)
- `zlink.lib` - Import library for linking
- `msvcp140.dll` - VC++ runtime (matching architecture)
- `vcruntime140.dll` - VC++ runtime (matching architecture)
- `vcruntime140_1.dll` - VC++ runtime (matching architecture, if available)

## Important Notes for ARM64 Builds

### Cross-Compilation Limitations

1. **Cannot execute on build host**: ARM64 binaries built on x64 machines cannot be executed on the x64 host
2. **No test execution**: The build script disables tests (`-DBUILD_TESTS=OFF`) since they cannot run on x64
3. **Deployment required**: To test ARM64 binaries, you must:
   - Copy the DLLs to an ARM64 Windows device (e.g., Surface Pro X, Windows on ARM)
   - Run your tests on the ARM64 device

### Verification

The script uses `dumpbin /headers` to verify the target architecture. For ARM64 builds, you should see:

```
AA64 machine (ARM64)
```

For x64 builds, you should see:

```
8664 machine (x64)
```

## Build Directory Structure

```
libzlink-native/
├── build/
│   ├── windows-x64/         # x64 build artifacts
│   └── windows-arm64/       # ARM64 build artifacts
├── deps/
│   ├── vcpkg/               # Shared vcpkg installation
│   ├── windows-x64/         # x64 dependencies
│   │   └── zlink-6.0.3/
│   └── windows-arm64/       # ARM64 dependencies
│       └── zlink-6.0.3/
└── core/dist/
    ├── windows-x64/         # x64 output binaries
    │   ├── zlink.dll
    │   ├── zlink.lib
    │   └── *.dll (runtime)
    └── windows-arm64/       # ARM64 output binaries
        ├── zlink.dll
        ├── zlink.lib
        └── *.dll (runtime)
```

## Troubleshooting

### Error: "ARM64 compiler not found"

**Solution:** Install ARM64 build tools in Visual Studio Installer (see Prerequisites section)

### Warning: "Cannot execute ARM64 binary"

**Expected behavior:** This is normal when building ARM64 on x64. The binaries must be deployed to an ARM64 device for testing.

## CI/CD Integration

For automated builds, you can build both architectures in parallel:

```powershell
# Build both architectures
$jobs = @(
    { .\core\builds\windows\build.ps1 -Architecture x64 },
    { .\core\builds\windows\build.ps1 -Architecture arm64 }
)

$jobs | ForEach-Object { Start-Job -ScriptBlock $_ }
Get-Job | Wait-Job | Receive-Job
```

## Parameter Reference

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `-Architecture` | String (x64/arm64) | `x64` | Target architecture for compilation |
| `-LibzlinkVersion` | String | `6.0.3` | Version label (defaults to VERSION file) |
| `-BuildType` | String | `Release` | CMake build type (Release/Debug) |
| `-OutputDir` | String | `dist\windows-{arch}` | Custom output directory for binaries |

## Examples

### Debug Build for ARM64

```powershell
.\core\builds\windows\build.ps1 -Architecture arm64 -BuildType Debug
```

### Specific Versions

```powershell
.\core\builds\windows\build.ps1 `
    -Architecture arm64 `
    -LibzlinkVersion "4.3.6"
```

### Custom Output Location

```powershell
.\core\builds\windows\build.ps1 `
    -Architecture arm64 `
    -OutputDir "my-custom-output"
```

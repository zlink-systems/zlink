[English](./build-guide.md) | [한국어](./build-guide.ko.md)

# Build Guide

## 1. Requirements

- **CMake** 3.10+
- **C++17** compiler (GCC 7+, Clang 5+, MSVC 2017+)
- **OpenSSL** (for TLS/WSS support)

## 2. Platform-Specific Builds

### 2.1 Linux

```bash
./core/builds/linux/build.sh x64 ON
```

ARM64:
```bash
./core/builds/linux/build.sh arm64 ON
```

### 2.2 macOS

```bash
./core/builds/macos/build.sh arm64 ON
```

x64:
```bash
./core/builds/macos/build.sh x64 ON
```

### 2.3 Windows (PowerShell)

```powershell
.\core\builds\windows\build.ps1 -Architecture x64 -RunTests "ON"
```

## 3. Direct CMake Build

```bash
cmake -S core -B core/build -DWITH_TLS=ON -DBUILD_TESTS=ON
cmake --build core/build
ctest --test-dir core/build --output-on-failure
```

## 4. Quick Build (without tests)

```bash
cmake -S core -B core/build -DBUILD_TESTS=OFF && cmake --build core/build
```

## 5. Core Build Script

```bash
./core/build.sh
```

Runs a clean CMake build with tests.

## 6. OpenSSL Installation

### Ubuntu/Debian
```bash
sudo apt-get install libssl-dev
```

### macOS
```bash
brew install openssl@3
```

### Windows (vcpkg)
```bash
vcpkg install openssl:x64-windows
```

## 7. CMake Options

For detailed options, see [cmake-options.md](./cmake-options.md).

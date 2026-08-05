# C++ Standard Build Examples for zlink

## Overview

zlink supports building with multiple C++ standards via the `ZLINK_CXX_STANDARD` CMake option.

## Build Options

### Linux/macOS with C++17 (default)

```bash
# Configure with C++17
cmake -S core -B core/build -DZLINK_CXX_STANDARD=17

# Build
cmake --build core/build
```

### Windows with C++17 (Visual Studio)

```powershell
# Configure with C++17
cmake -S core -B core/build -DZLINK_CXX_STANDARD=17

# Build
cmake --build core/build --config Release
```

### Supported C++ Standards

- **11** - C++11 (옵션으로 선택 가능)
- **14** - C++14
- **17** - C++17 (default if no option specified)
- **20** - C++20
- **23** - C++23 (if supported by compiler)
- **latest** - Latest supported by compiler (MSVC only)

### Examples with Different Standards

```bash
# C++14
cmake -S core -B core/build -DZLINK_CXX_STANDARD=14

# C++17
cmake -S core -B core/build -DZLINK_CXX_STANDARD=17

# C++20
cmake -S core -B core/build -DZLINK_CXX_STANDARD=17

# C++23 (if compiler supports)
cmake -S core -B core/build -DZLINK_CXX_STANDARD=23
```

### Combined with Other Options

```bash
# C++17 with tests enabled
cmake -S core -B core/build -DZLINK_CXX_STANDARD=17 -DBUILD_TESTS=ON

# C++17 with benchmarks enabled
cmake -S core -B core/build -DZLINK_CXX_STANDARD=17 -DBUILD_BENCHMARKS=ON

# C++17 with Release build type
cmake -S core -B core/build -DZLINK_CXX_STANDARD=17 -DCMAKE_BUILD_TYPE=Release
```

### Using Build Scripts

#### Linux

```bash
# Modify core/builds/linux/build.sh to add:
# cmake -S "$REPO_ROOT/core" -B "$BUILD_DIR" \
#   -DZLINK_CXX_STANDARD=17 \
#   -DCMAKE_BUILD_TYPE=Release \
#   ...

./core/builds/linux/build.sh x64 ON
```

#### macOS

```bash
# Modify core/builds/macos/build.sh similarly
./core/builds/macos/build.sh arm64 ON
```

#### Windows

```powershell
# Modify core/builds/windows/build.ps1 to add:
# cmake -B "$buildDir" `
#   -DZLINK_CXX_STANDARD=17 `
#   -G "Visual Studio 17 2022" `
#   ...

.\core\builds\windows\build.ps1 -Architecture x64 -RunTests "ON"
```

## Compiler Requirements

### Minimum Compiler Versions for C++17

| Compiler | Minimum Version |
|----------|----------------|
| GCC      | 7.0            |
| Clang    | 5.0            |
| MSVC     | 19.14 (VS 2017 15.7) |
| Apple Clang | 10.0       |

### Verification

Check if your compiler supports C++17:

```bash
# GCC
g++ --version
g++ -std=c++17 -E -x c++ - < /dev/null > /dev/null 2>&1 && echo "C++17 supported" || echo "C++17 not supported"

# Clang
clang++ --version
clang++ -std=c++17 -E -x c++ - < /dev/null > /dev/null 2>&1 && echo "C++17 supported" || echo "C++17 not supported"

# MSVC (Windows)
cl.exe /?
```

## Fallback Behavior

- If the specified C++ standard is not supported by the compiler, the build will:
  - **Linux/macOS**: Display a warning and fall back to C++11
  - **MSVC**: Display a warning and use compiler default

- If `ZLINK_CXX_STANDARD` is not specified, the build uses:
  - **Linux/macOS**: C++17
  - **MSVC**: C++17 (`/std:c++17`)

## Notes

1. **Binary Compatibility**: Changing the C++ standard may affect ABI compatibility. Ensure all dependent libraries use compatible standards.

2. **Feature Usage**: zlink's core code targets C++17. Higher standards allow users to link zlink with their C++20/23 projects without ABI issues.

3. **CMake Cache**: If changing standards, clean the build directory or use `-U ZLINK_CXX_STANDARD` to force reconfiguration:
   ```bash
   cmake -S core -B core/build -U ZLINK_CXX_STANDARD -DZLINK_CXX_STANDARD=17
   ```

4. **Per-Platform Differences**:
- GCC/Clang use `-std=c++17`
- MSVC uses `/std:c++17`
   - The CMake configuration handles these differences automatically

## Troubleshooting

### Error: Compiler doesn't support C++17

```
CMake Warning:
  Compiler does not support C++17, falling back to C++11
```

**Solution**: Update your compiler to a version that supports C++17 (see table above).

### Error: Unknown flag `-std=c++17`

**Solution**: Your compiler is too old. Update to a newer version or use a lower C++ standard:

```bash
cmake -S core -B core/build -DZLINK_CXX_STANDARD=14
```

### Cache issues when switching standards

**Solution**: Clean build directory or remove CMake cache:

```bash
rm -rf build
cmake -S core -B core/build -DZLINK_CXX_STANDARD=17
```

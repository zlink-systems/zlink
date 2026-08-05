[English](./cmake-options.md) | [한국어](./cmake-options.ko.md)

# CMake Options Reference

## 1. Basic Options

| Option | Default | Description |
|--------|---------|-------------|
| `WITH_TLS` | `ON` | Enable TLS/WSS via OpenSSL |
| `BUILD_TESTS` | `ON` | Build tests |
| `BUILD_BENCHMARKS` | `OFF` | Build benchmarks |
| `BUILD_SHARED` | `ON` | Build shared library |
| `ZLINK_CXX_STANDARD` | `17` | C++ standard (11, 14, 17, 20, 23) |

## 2. Usage Examples

### Build without TLS
```bash
cmake -S core -B core/build -DWITH_TLS=OFF
```

### Use C++20 standard
```bash
cmake -S core -B core/build -DZLINK_CXX_STANDARD=20
```

### Include benchmarks
```bash
cmake -S core -B core/build -DBUILD_BENCHMARKS=ON
```

### Static library build
```bash
cmake -S core -B core/build -DBUILD_SHARED=OFF
```

## 3. Advanced Options

Options for debugging and static analysis:

| Option | Description |
|--------|-------------|
| `ENABLE_ASAN` | Enable AddressSanitizer |
| `ENABLE_TSAN` | Enable ThreadSanitizer |
| `ENABLE_UBSAN` | Enable UndefinedBehaviorSanitizer |
| `ENABLE_LTO` | Enable Link-Time Optimization |

```bash
cmake -S core -B core/build -DENABLE_ASAN=ON -DBUILD_TESTS=ON
cmake --build core/build
ctest --test-dir core/build
```

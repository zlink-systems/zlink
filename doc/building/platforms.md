[English](./platforms.md) | [한국어](./platforms.ko.md)

# Supported Platforms and Compilers

## 1. Supported Platforms

| Platform | Architecture | Status |
|----------|:------------:|:------:|
| Linux | x64, ARM64 | Stable |
| macOS | x64, ARM64 | Stable |
| Windows | x64, ARM64 | Stable |

## 2. Compiler Requirements

| Compiler | Minimum Version |
|----------|----------------|
| GCC | 13+ |
| Clang | 5+ |
| MSVC | 2017+ (19.14+) |

## 3. C++ Standard

- Default: **C++17**
- Supported: C++11, C++14, C++17, C++20, C++23
- Configuration: `cmake -DZLINK_CXX_STANDARD=17`

## 4. Dependencies

| Dependency | Purpose | Required |
|------------|---------|----------|
| OpenSSL | TLS/WSS support | Optional (`WITH_TLS=ON`) |
| Boost.Asio | I/O engine | Bundled (no external dependency) |

## 5. IPC Transport Limitation

- `ipc://` transport is only supported on **Unix/Linux/macOS**
- On Windows, use `tcp://` or `inproc://` instead

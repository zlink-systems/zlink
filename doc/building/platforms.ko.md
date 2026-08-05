[English](./platforms.md) | [한국어](./platforms.ko.md)

# 지원 플랫폼 및 컴파일러

## 1. 지원 플랫폼

| 플랫폼 | Architecture | 상태 |
|--------|:------------:|:----:|
| Linux | x64, ARM64 | Stable |
| macOS | x64, ARM64 | Stable |
| Windows | x64, ARM64 | Stable |

## 2. 컴파일러 요구사항

| 컴파일러 | 최소 버전 |
|----------|----------|
| GCC | 13+ |
| Clang | 5+ |
| MSVC | 2017+ (19.14+) |

## 3. C++ 표준

- 기본: **C++17**
- 지원: C++11, C++14, C++17, C++20, C++23
- 설정: `cmake -DZLINK_CXX_STANDARD=17`

## 4. 의존성

| 의존성 | 용도 | 필수 여부 |
|--------|------|----------|
| OpenSSL | TLS/WSS 지원 | 선택 (`WITH_TLS=ON`) |
| Boost.Asio | I/O 엔진 | 번들 (외부 의존 없음) |

## 5. IPC Transport 제한

- `ipc://` transport는 **Unix/Linux/macOS**에서만 지원
- Windows에서는 `tcp://` 또는 `inproc://` 사용

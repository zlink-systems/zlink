[English](./cmake-options.md) | [한국어](./cmake-options.ko.md)

# CMake 옵션 상세

## 1. 기본 옵션

| 옵션 | 기본값 | 설명 |
|------|--------|------|
| `WITH_TLS` | `ON` | OpenSSL을 통한 TLS/WSS 활성화 |
| `BUILD_TESTS` | `ON` | 테스트 빌드 |
| `BUILD_BENCHMARKS` | `OFF` | 벤치마크 빌드 |
| `BUILD_SHARED` | `ON` | Shared Library 빌드 |
| `ZLINK_CXX_STANDARD` | `17` | C++ 표준 (11, 14, 17, 20, 23) |

## 2. 사용 예시

### TLS 없이 빌드
```bash
cmake -S core -B core/build -DWITH_TLS=OFF
```

### C++20 표준 사용
```bash
cmake -S core -B core/build -DZLINK_CXX_STANDARD=20
```

### 벤치마크 포함
```bash
cmake -S core -B core/build -DBUILD_BENCHMARKS=ON
```

### Static Library 빌드
```bash
cmake -S core -B core/build -DBUILD_SHARED=OFF
```

## 3. 고급 옵션

디버깅 및 정적 분석용 옵션:

| 옵션 | 설명 |
|------|------|
| `ENABLE_ASAN` | AddressSanitizer 활성화 |
| `ENABLE_TSAN` | ThreadSanitizer 활성화 |
| `ENABLE_UBSAN` | UndefinedBehaviorSanitizer 활성화 |
| `ENABLE_LTO` | Link-Time Optimization 활성화 |

```bash
cmake -S core -B core/build -DENABLE_ASAN=ON -DBUILD_TESTS=ON
cmake --build core/build
ctest --test-dir core/build
```

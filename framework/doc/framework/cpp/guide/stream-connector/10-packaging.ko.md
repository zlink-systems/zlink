# 10 — 패키징

[← 엔진 어댑터](09-engine-adapters.ko.md) | [목차](INDEX.ko.md) | [다음: 성능 테스트 →](11-performance.ko.md)

---

## 배포 단위

| 산출물 | vcpkg 패키지 명 | Conan 패키지 명 | 배포 형식 |
|--------|----------------|----------------|-----------|
| core connector | `zlink-stream-connector` | `zlink-stream-connector` | CMake, vcpkg, Conan |
| e2e client | `zlink-stream-e2e-client` | `zlink-stream-e2e-client` | CMake, vcpkg, Conan |
| Unreal plugin | — | — | source plugin |
| Godot adapter | — | — | source GDExtension |
| Axmol adapter | — | — | source package |

---

## vcpkg

### 설치

기본 설치 (TCP, JSON):

```bash
vcpkg install zlink-stream-connector
```

features를 추가하려면 bracket 표기를 사용한다:

```bash
vcpkg install "zlink-stream-connector[tls,websocket,lz4]"
```

e2e client도 함께 설치:

```bash
vcpkg install zlink-stream-connector zlink-stream-e2e-client
```

### 지원 features

| feature | 내용 | 의존성 |
|---------|------|--------|
| `tls` | TLS over TCP / WSS transport | OpenSSL |
| `websocket` | WebSocket / WSS transport | (Boost.Beast 포함) |
| `lz4` | LZ4 패킷 압축 | LZ4 |

### CMakeLists.txt

```cmake
find_package(zlink-stream-connector CONFIG REQUIRED)

target_link_libraries(my_game PRIVATE zlink::stream_connector)
```

---

## Conan

### conanfile.txt

```ini
[requires]
zlink-stream-connector/0.1.0

[options]
zlink-stream-connector/*:with_tls=True
zlink-stream-connector/*:with_websocket=True
zlink-stream-connector/*:with_lz4=True
```

### conanfile.py

```python
from conan import ConanFile

class MyGameConan(ConanFile):
    requires = "zlink-stream-connector/0.1.0"
    options = {"zlink-stream-connector/*:with_tls": True,
               "zlink-stream-connector/*:with_websocket": True}
```

### Conan options

| option | 기본값 | 의미 |
|--------|--------|------|
| `with_tls` | `False` | TLS/WSS transport |
| `with_websocket` | `False` | WebSocket/WSS transport |
| `with_lz4` | `True` | LZ4 압축 |

---

## CMake FetchContent

vcpkg/Conan 없이 소스에서 직접 빌드한다.

```cmake
include(FetchContent)

FetchContent_Declare(zlink_stream_connector
    GIT_REPOSITORY https://github.com/ulala-x/zlink.git
    GIT_TAG        main
    SOURCE_SUBDIR  framework/languages/cpp/connector/core
)
set(ZLINK_STREAM_CONNECTOR_WITH_TLS        ON  CACHE BOOL "")
set(ZLINK_STREAM_CONNECTOR_WITH_WEBSOCKET  ON  CACHE BOOL "")
set(ZLINK_STREAM_CONNECTOR_WITH_LZ4        ON  CACHE BOOL "")

FetchContent_MakeAvailable(zlink_stream_connector)

target_link_libraries(my_game PRIVATE zlink::stream_connector)
```

### CMake build options

| option | 기본값 | 의미 |
|--------|--------|------|
| `ZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT` | `ON` | e2e client 빌드 포함 |
| `ZLINK_STREAM_CONNECTOR_WITH_TLS` | `ON` | TLS/WSS transport |
| `ZLINK_STREAM_CONNECTOR_WITH_WEBSOCKET` | `ON` | WebSocket/WSS transport |
| `ZLINK_STREAM_CONNECTOR_WITH_LZ4` | `ON` | LZ4 압축 |
| `ZLINK_STREAM_CONNECTOR_BUILD_UNREAL` | `OFF` | Unreal adapter 빌드 |
| `ZLINK_STREAM_CONNECTOR_BUILD_GODOT` | `OFF` | Godot adapter 빌드 |
| `ZLINK_STREAM_CONNECTOR_BUILD_AXMOL` | `OFF` | Axmol adapter 빌드 |

---

## CMake target 구성

| target | public include | 용도 |
|--------|---------------|------|
| `zlink::stream_connector` | `zlink/stream_connector.hpp` | core connector |
| `zlink::stream_e2e_client` | `zlink/stream_e2e_client.hpp` | e2e/perf scenario |

두 target은 상호 독립적이다. `zlink::stream_e2e_client`는 `zlink::stream_connector`를 public 또는 private dependency로 사용한다.

---

## 엔진 어댑터 배포

Unreal, Godot, Axmol 어댑터는 vcpkg/Conan이 아닌 source package로 배포한다.

| 어댑터 | 배포 방식 | 설명 |
|--------|---------|------|
| Unreal | source plugin (`.uplugin`) | `Plugins/ZLinkStreamConnector/`에 배치 |
| Godot | source GDExtension | `addons/zlink_stream_connector/`에 배치 |
| Axmol | CMake source | `third_party/` 또는 FetchContent |

각 어댑터는 core connector를 내부 ThirdParty로 포함하거나 설치된 CMake package를 참조한다. 어댑터 package 사용자가 서버 framework package를 설치할 필요는 없다.

---

## 의존성 요약

| 의존성 | 용도 | 기본 포함 |
|--------|------|-----------|
| Boost.Asio | 비동기 I/O runtime | 항상 |
| Boost.Beast | WebSocket | `WITH_WEBSOCKET` |
| nlohmann/json | JSON codec | 항상 |
| OpenSSL | TLS/WSS | `WITH_TLS` |
| LZ4 | 압축 (system LZ4 또는 fallback source) | `WITH_LZ4` |
| msgpack-cxx | MessagePack codec | `WITH_MESSAGEPACK` |
| protobuf | Protobuf codec | `WITH_PROTOBUF` |

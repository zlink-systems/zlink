# 10 — 패키징

[← 엔진 어댑터](09-engine-adapters.ko.md) | [목차](INDEX.ko.md) | [다음: 성능 테스트 →](11-performance.ko.md)

---

## 배포 단위

| 산출물 | vcpkg 패키지 명 | Conan 패키지 명 | 배포 형식 |
|--------|----------------|----------------|-----------|
| core connector | `zlink-stream-connector` | `zlink-stream-connector` | CMake, vcpkg, Conan |
| e2e client | `zlink-stream-e2e-client` | `zlink-stream-e2e-client` | CMake, vcpkg, Conan |
| Unreal plugin | — | — | source plugin + generated ThirdParty package |
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
zlink-stream-connector/0.10.0

[options]
zlink-stream-connector/*:with_tls=True
zlink-stream-connector/*:with_websocket=True
zlink-stream-connector/*:with_lz4=True
```

### conanfile.py

```python
from conan import ConanFile

class MyGameConan(ConanFile):
    requires = "zlink-stream-connector/0.10.0"
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
    GIT_REPOSITORY https://github.com/zlink-systems/zlink.git
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
| `ZLINK_STREAM_CONNECTOR_BUILD_UNREAL` | `ON` | Unreal adapter 빌드 |
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
| Unreal | source plugin (`.uplugin`) + generated native package | `Plugins/ZLinkStreamConnector/`에 배치하고 `ThirdParty/ZLink/`를 생성 |
| Godot | source GDExtension | `addons/zlink_stream_connector/`에 배치 |
| Axmol | CMake source | `third_party/` 또는 FetchContent |

각 어댑터는 core connector를 내부 ThirdParty로 포함하거나 설치된 CMake package를 참조한다. 어댑터 package 사용자가 서버 framework package를 설치할 필요는 없다.

Unreal plugin은 CMake target graph를 Unreal Build Tool에 직접 전달할 수 없으므로,
`Tools/package-third-party.cmake`가 configured CMake build에서 native package를 생성한다.
다음 명령은 Unreal adapter, C++ binding, Core runtime과 Core CMake package, 선택된
OpenSSL/LZ4 library와 상대 경로 manifest를 `ThirdParty/ZLink/`에 설치한다.

```bash
cmake \
  -DZLINK_UNREAL_BUILD_DIR=/absolute/path/to/framework/languages/cpp/build \
  -DZLINK_UNREAL_OUTPUT_DIR=/absolute/path/to/ThirdParty/ZLink \
  -DZLINK_UNREAL_CONFIGURATION=Release \
  -P framework/languages/cpp/connector/engines/unreal/Tools/package-third-party.cmake
```

`ZLinkStreamConnector.Build.cs`는 manifest를 읽어 include path, link library,
runtime file과 system library를 검증한 뒤 Unreal module에 등록한다. 패키지 경로를
plugin 밖에 둘 때는 `ZLINK_UNREAL_THIRDPARTY_ROOT`를 manifest 디렉터리로 지정한다.
Windows의 `ws2_32`, `mswsock`와 TLS에 필요한 system library도 manifest에 기록된다.
이는 Asio runtime source에 platform branch를 추가하는 방식이 아니라 build/package
단계에서 link dependency를 전달하는 방식이다. script는 `StreamConnector` CMake
component만 설치하므로 server framework와 HTTP client 산출물을 섞지 않는다. manifest에는
platform, architecture, configuration, compiler와 C++ standard를 기록하며, Unreal
module이 target과 다른 package를 사용하면 build를 중단한다. 설치된 CMake export는
LZ4 dependency를 package prefix 기준으로 참조하고 Windows Core import library도 함께
포함하므로 producer의 build 디렉터리에 의존하지 않는다.

Unreal Build Tool을 실행하기 전에 Unreal target이 실제로 선택한 compiler 값으로
`ZLINK_UNREAL_COMPILER_ID`와 `ZLINK_UNREAL_COMPILER_VERSION`을 설정해야 한다. 두 값은
manifest와 일치해야 하며, package를 다른 compiler 또는 CRT로 연결하려는 경우 build를
중단한다. system library 목록은 package 대상 platform과 Core export에서 결정하므로
package를 생성하는 host platform과 다른 target도 같은 manifest 규칙을 사용한다.
module은 `ReadOnlyTargetRules`의 `Platform`, `Architecture`, `Configuration` 속성을
사용하므로, 사용하는 Unreal version의 실제 UBT에서 이 API와 link를 함께 검증해야 한다.

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

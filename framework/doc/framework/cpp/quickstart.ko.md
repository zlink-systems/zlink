# C++ Quickstart — 빈 프로젝트에서 첫 요청까지

> **이 장의 계약 소유 문서** — 없다. API의 정식 계약은
> [C++ 스펙](../common/spec/server/languages/cpp/README.ko.md)이 다룬다.

저장소의 [`framework/languages/cpp/quickstart/`](../../../languages/cpp/quickstart/)
프로젝트다. 아래 코드 블록은 사이트를 빌드할 때 그 파일에서 읽는다.

location store 없이 process 둘이 서로의 endpoint를 직접 지정해 request/reply 한 번을
주고받는다. 다음 단계는 [설치와 첫 동작](guide/server/02-getting-started.ko.md)이다.

## 설치 경로

C++은 세 가지 경로가 있다. 셋 다 같은 결과를 낸다 — 무엇을 이미 쓰고 있는지로 고른다.

| 경로 | 언제 |
|---|---|
| vcpkg | 이미 vcpkg를 쓰는 프로젝트 |
| Conan | 이미 Conan을 쓰는 프로젝트 |
| GitHub Release | 패키지 관리자를 쓰지 않을 때. source archive 세 개를 차례로 빌드한다 |

`zlink`는 아직 공식 vcpkg 레지스트리와 ConanCenter에 없다. 이 저장소가 제공하는 overlay
port와 recipe를 쓴다.

### vcpkg

```bash
git clone https://github.com/zlink-systems/zlink.git
vcpkg install zlink zlink-cpp zlink-framework \
  --overlay-ports=zlink/vcpkg/ports --triplet=x64-linux
```

소비자 프로젝트는 vcpkg toolchain을 쓴다.

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
```

### Conan

```bash
git clone https://github.com/zlink-systems/zlink.git
conan create zlink/core/packaging/conan --build=missing -s compiler.cppstd=gnu20
conan create zlink/bindings/cpp/packaging/conan --build=missing -s compiler.cppstd=gnu20
conan create zlink/framework/languages/cpp/packaging/conan --build=missing -s compiler.cppstd=gnu20
```

소비자 프로젝트의 `conanfile.txt`에 `zlink-framework/0.14.0`을 적고 `conan install`한다.

### GitHub Release

세 아카이브를 차례로 빌드해 설치한다 — `core/vX.Y.Z` → `cpp/vX.Y.Z` → `framework-cpp/vA.B.C`.
순서와 실제 명령은 프로젝트의
[`README.md`](../../../languages/cpp/quickstart/README.md)에 있다.

이 경로에서는 서드파티를 직접 갖춘다. `nlohmann_json`·Boost·liblz4·libprotobuf·OpenSSL은
배포판 패키지로 설치하고, `opentelemetry-cpp`는 배포판 패키지가 없어 소스로 빌드한다.
framework가 링크하는 것은 `opentelemetry-cpp::api` 하나이므로 `-DOTELCPP_WITH_API_ONLY=ON`이면
헤더만 빌드된다.

## 전제

- CMake 3.20 이상, C++20 컴파일러

## 1. 소비자 쪽 CMake

세 단계 설치가 끝나면 소비자가 적을 것은 이게 전부다.

```cmake title="CMakeLists.txt"
--8<-- "framework/languages/cpp/quickstart/CMakeLists.txt"
```

## 2. 공유 계약

메시지 타입에 `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE`가 필요하다. 기본 JSON serializer가
ADL로 `to_json`/`from_json`을 찾으므로 bare struct는 직렬화되지 않는다.

```cpp title="Shared/messages.hpp"
--8<-- "framework/languages/cpp/quickstart/Shared/messages.hpp"
```

## 3. 처리하는 쪽

`object_role` 기본값이 `server`라 그대로 두면 location store를 요구한다. 이 구성에서는
`none`으로 지정한다. `routing_id`와, wildcard bind host를 쓸 때의 `advertise_host`도
필수다.

```cpp title="Server/main.cpp"
--8<-- "framework/languages/cpp/quickstart/Server/main.cpp"
```

## 4. 호출하는 쪽

HTTP handler는 경로 파라미터를 인자로 받지 않는다. `http_request_t`를 받아
`request.route_values`에서 꺼낸다.

```cpp title="Client/main.cpp"
--8<-- "framework/languages/cpp/quickstart/Client/main.cpp"
```

## 5. 실행

```bash
cd framework/languages/cpp/quickstart
cmake -S . -B build -DCMAKE_PREFIX_PATH=<framework install prefix>
cmake --build build

# 터미널 두 개. server를 먼저 실행한다.
./build/quickstart_server
./build/quickstart_client

curl http://127.0.0.1:5083/hello/world
```

응답은 `"hello, world"`, 상태 코드 200이다.

## 6. Visual Studio 2022

Windows에서는 CMake 명령 대신 Visual Studio로 열어도 된다. 필요한 것은 **Desktop
development with C++** 워크로드와 그 안의 **C++ CMake tools for Windows** 구성 요소다.

1. 3단계 설치를 먼저 마친다. Windows에서도 순서는 같고 PowerShell에서 실행한다.
2. framework install prefix를 환경 변수에 넣는다. Visual Studio는 이 값을 preset의
   `CMAKE_PREFIX_PATH`로 읽는다.

    ```powershell
    $env:ZLINK_PREFIX = "C:\zlink\install\framework"
    ```

3. **파일 → 열기 → 폴더**로 `framework/languages/cpp/quickstart`를 연다. 솔루션 파일을
   만들 필요는 없다. Visual Studio가 `CMakePresets.json`을 읽는다.
4. 도구 모음의 구성 드롭다운에서 **`Visual Studio 2022 (x64)`** 를 고른다. vcpkg로 의존성을
   설치했다면 **`Visual Studio 2022 (x64, vcpkg toolchain)`** 을 고른다. 이쪽은
   `VCPKG_ROOT`를 읽는다.
5. **빌드 → 모두 빌드.**
6. 시작 항목에서 `quickstart_server`를 골라 실행하고, 별도의 Developer PowerShell에서
   클라이언트를 실행한다. 한 번에 한 항목만 디버깅할 수 있으므로 두 process를 모두 Visual
   Studio에서 띄우지는 않는다.

    ```powershell
    .\build\vs2022\Debug\quickstart_client.exe
    curl http://127.0.0.1:5083/hello/world
    ```

preset은 프로젝트의 `CMakePresets.json`에 있다. 값을 바꿔야 하면 그 파일을 고치지 말고 같은
디렉터리에 `CMakeUserPresets.json`을 두는 쪽이 낫다 — 그 파일은 버전 관리 대상이 아니다.

```json title="CMakePresets.json"
--8<-- "framework/languages/cpp/quickstart/CMakePresets.json"
```

## 옮겨 갈 것

| 파일 | 내용 |
|---|---|
| `CMakeLists.txt` | `find_package(zlink_framework CONFIG REQUIRED)`와 `zlink::framework` 링크 |
| `Shared/messages.hpp` | `packet_name`과 `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE` |
| `Server/main.cpp` | `add_route_mesh` → `listen` → `set_object_role(none)`·`set_routing_id`·`set_advertise_host` → handler 등록 |
| `Client/main.cpp` | client 역할 지정, `peer_connections().connect(...)`, `request_to_channel(...)` |

수동 연결 대신 location store를 쓰는 구성은
[10. Location](guide/server/10-location.ko.md)이 다룬다.

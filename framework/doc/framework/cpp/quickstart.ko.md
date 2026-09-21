# C++ Quickstart — 설치부터 첫 요청까지

!!! info "이 장을 읽고 나면"

    패키지를 설치하고, 두 process가 서로 호출하는 최소 project를 실행할 수 있다.

저장소의 [`framework/languages/cpp/quickstart/`](../../../languages/cpp/quickstart/)
프로젝트다. 아래 코드 블록은 사이트를 빌드할 때 그 파일에서 읽는다. location store 없이
process 둘이 서로의 endpoint를 직접 지정해 request/reply 한 번을 주고받는다.

## 0. 예제 저장소 clone

이 장의 project는 `zlink-cpp-examples` 저장소의 `quickstart/`이며, 기능 가이드가 읽는
program인 `tutorial/`과 `samples/`도 같은 저장소에 있다.

```bash
git clone https://github.com/zlink-systems/zlink-cpp-examples.git
cd zlink-cpp-examples/quickstart
```

`main`은 최신 릴리스에 그 뒤의 수정을 더한 것이고, 패키지 버전은 그 릴리스에 맞춰져 있다.
이전 릴리스는 tag `vA.B.C`로 받는다(`git checkout vA.B.C`). 이슈와 PR은 `zlink-systems/zlink`로
보낸다.

## 1. 설치

- CMake 3.20 이상, C++20 컴파일러. framework가 C++20 coroutine을 사용한다.
  `CMakePresets.json`을 사용하는 [Visual Studio 2022](#7-visual-studio-2022) 경로는 3.21 이상이 필요하다
- nlohmann_json·Boost·liblz4·libprotobuf·OpenSSL·opentelemetry-cpp

`zlink`는 아직 공식 vcpkg registry와 ConanCenter에 없다. 이 저장소가 overlay port와 Conan
recipe를 함께 담고 있으므로 설치 경로는 셋이다. **끝까지 검증한 것은 GitHub Release 경로다.**
`zlink-cpp-examples`의 `quickstart/`·`tutorial/`·`samples/`에 있는 `bootstrap.cmake`가 이 경로를
자동화한다.

| 경로 | 지금 상태 |
|---|---|
| GitHub Release | Core prebuilt 하나와 source archive 둘을 차례로 설치한다. Windows·Linux에서 끝까지 확인했다 |
| vcpkg overlay port | 릴리스마다 `sync-recipes`가 갱신한다. 이 문서의 검증 범위 밖이다 |
| Conan recipe | 릴리스마다 `sync-recipes`가 갱신한다. 이 문서의 검증 범위 밖이다 |

아카이브 경로의 명령은 프로젝트의
[`README.md`](../../../languages/cpp/quickstart/README.md)에 있다.

### 1.1 vcpkg

!!! note "검증 범위 밖"

    이 경로는 릴리스마다 갱신되지만 이 문서가 검증한 경로가 아니다. 첫 설치는
    [GitHub Release](#13-github-release)로 한다.

```bash
git clone https://github.com/zlink-systems/zlink.git
vcpkg install zlink zlink-cpp zlink-framework \
  --overlay-ports=zlink/vcpkg/ports --triplet=x64-linux
```

소비자 프로젝트는 vcpkg toolchain을 사용한다.

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
```

### 1.2 Conan

!!! note "검증 범위 밖"

    이 경로는 릴리스마다 갱신되지만 이 문서가 검증한 경로가 아니다. 첫 설치는
    [GitHub Release](#13-github-release)로 한다.

```bash
git clone https://github.com/zlink-systems/zlink.git
conan create zlink/core/packaging/conan --build=missing -s compiler.cppstd=gnu20
conan create zlink/bindings/cpp/packaging/conan --build=missing -s compiler.cppstd=gnu20
conan create zlink/framework/languages/cpp/packaging/conan --build=missing -s compiler.cppstd=gnu20
```

소비자 project의 `conanfile.txt`에 `zlink-framework/0.16.0`을 적고 `conan install`한다.

### 1.3 GitHub Release

세 아카이브를 차례로 빌드해 설치한다 — `core/vX.Y.Z` → `cpp/vX.Y.Z` → `framework-cpp/vA.B.C`.
순서와 실제 명령은 프로젝트의
[`README.md`](../../../languages/cpp/quickstart/README.md)에 있다.

이 경로에서는 third-party 의존을 직접 설치한다. `nlohmann_json`·Boost·liblz4·libprotobuf·OpenSSL은
배포판 패키지로 설치하고, `opentelemetry-cpp`는 배포판 패키지가 없어 소스로 빌드한다.
framework가 링크하는 것은 `opentelemetry-cpp::api` 하나이므로 `-DOTELCPP_WITH_API_ONLY=ON`이면
헤더만 빌드된다.

### 1.4 필요할 때 추가하는 target

| target | 언제 추가하나 |
| --- | --- |
| `zlink::framework_locations_redis` | Redis location store로 자동 연결을 사용할 때([Location](guide/server/25-location.ko.md)) |
| `zlink::framework_codec_protobuf` · `_messagepack` | 기본 JSON codec 대신 사용할 때([Handler와 메시지 처리](guide/server/31-handler-dispatch.ko.md#3-codec--payload를-바이트로-바꾼다)) |
| `zlink::stream_connector` | 외부 client(게임 client·모바일)를 만들 때([STREAM](guide/server/23-stream.ko.md)) |
| `zlink::http_client` | 서버에서 HTTP를 호출할 때([HTTP Client 가이드](guide/http-client/README.ko.md)) |

라이선스는 계층마다 다르다 — core·binding은 MPL-2.0, framework는 FSL-1.1-ALv2,
`zlink::http_client`는 Apache-2.0이다. 서비스를 만들어 파는 데 드는 비용은 없다
([ZLink의 적용 범위](guide/server/17-alternative.ko.md#8-라이선스--사용하는-데-드는-비용)).

## 2. 소비자 쪽 CMake

세 단계 설치가 끝나면 소비자 project의 CMake는 다음과 같다.

```cmake title="CMakeLists.txt"
--8<-- "framework/languages/cpp/quickstart/CMakeLists.txt"
```

## 3. 공유 계약

메시지 타입에 `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE`가 필요하다. 기본 JSON serializer가
ADL로 `to_json`/`from_json`을 찾으므로 bare struct는 직렬화되지 않는다.

```cpp title="Shared/messages.hpp"
--8<-- "framework/languages/cpp/quickstart/Shared/messages.hpp"
```

## 4. 처리하는 쪽

`object_role` 기본값이 `server`라 그대로 두면 location store를 요구한다. 이 구성에서는
`none`으로 지정한다. `routing_id`는 필수다. `0.0.0.0`으로 bind하고 `advertise_host`를 생략하면 같은 address family의
loopback(`127.0.0.1`)을 광고한다. Container나 여러 host에서 remote process가 그 loopback으로 접속할
수 없으면 접속 가능한 `advertise_host`를 지정한다.

```cpp title="Server/main.cpp"
--8<-- "framework/languages/cpp/quickstart/Server/main.cpp"
```

## 5. 호출하는 쪽

HTTP handler는 경로 파라미터를 인자로 받지 않는다. `http_request_t`를 받아
`request.route_values`에서 꺼낸다.

```cpp title="Client/main.cpp"
--8<-- "framework/languages/cpp/quickstart/Client/main.cpp"
```

## 6. 실행

`bootstrap.cmake`가 §1의 설치를 대신하고 이 project를 `build/`에 구성한다. 첫 실행은 vcpkg가
서드파티를 빌드하므로 20분 정도 걸리고, `tutorial/`을 먼저 bootstrap했으면
`cmake -DZLINK_ROOT=../tutorial/.zlink -P bootstrap.cmake`로 그 결과를 재사용한다.

```bash
cd zlink-cpp-examples/quickstart
cmake -P bootstrap.cmake
cmake --build build --parallel

# 터미널 두 개. server를 먼저 실행한다.
./build/quickstart_server
./build/quickstart_client

curl http://127.0.0.1:5083/hello/world
```

§1의 세 단계를 직접 설치했다면 bootstrap 대신 `cmake -S . -B build -DCMAKE_PREFIX_PATH=<framework
install prefix>`로 구성한다. 응답은 `"hello, world"`, 상태 코드 200이다.

## 7. Visual Studio 2022

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

## 8. 첫 실행이 안 될 때 확인할 항목

| 증상 | 확인할 항목 |
| --- | --- |
| `find_package`가 실패한다 | `CMAKE_PREFIX_PATH`가 framework install prefix를 가리키는지, 세 단계 설치를 모두 마쳤는지 확인한다 |
| server가 location store를 요구한다 | `set_object_role`을 `none`으로 지정했는지 확인한다 |
| startup이 실패한다 | 두 process의 mesh 이름이 같은지, `routing_id`를 지정했는지 확인한다 |
| 메시지가 직렬화되지 않는다 | 메시지 타입에 `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE`를 붙였는지 확인한다 |
| 호출이 대상 없음으로 끝난다 | 받는 쪽이 그 channel 이름을 server 역할로 등록했는지, 두 process가 peer로 연결됐는지 확인한다 |

## 9. 옮겨 갈 것

| 파일 | 내용 |
|---|---|
| `CMakeLists.txt` | `find_package(zlink_framework CONFIG REQUIRED)`와 `zlink::framework` 링크 |
| `Shared/messages.hpp` | `packet_name`과 `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE` |
| `Server/main.cpp` | `add_route_mesh` → `listen` → `set_object_role(none)`·`set_routing_id`·`set_advertise_host` → handler 등록 |
| `Client/main.cpp` | client 역할 지정, `peer_connections().connect(...)`, `request_to_channel(...)` |

## 10. 다음으로 읽을 것

이 두 process는 endpoint를 서로 직접 적어 연결한다. 서버를 늘리거나 다른 주소로 다시 시작해도
호출 코드를 그대로 두려면 자동 연결이 필요하고, 그것은
[Location](guide/server/25-location.ko.md)이 다룬다.

- 개념을 먼저 확인할 때 — [핵심 개념](guide/server/03-concepts.ko.md)
- 이름으로 호출하는 경로 — [Channel 메시징](guide/server/20-channel-messaging.ko.md)
- id로 호출하는 상태 객체 — [Spot](guide/server/21-spot.ko.md) · [Actor](guide/server/22-actor.ko.md)
- 완결된 업무 흐름을 볼 때 — [샘플 고르기](guide/server/14-samples.ko.md)

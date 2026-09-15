# 설치

이 문서는 ZLink를 애플리케이션에 넣는 첫 단계다. framework는 각 언어의 패키지 저장소에 게시된
패키지로 설치한다. framework 패키지를 설치하면 그 framework가 의존하는 binding 패키지도 함께
설치된다. binding 패키지에는 Core 엔진의 native 런타임이 들어 있으므로 Core를 직접 빌드할
필요가 없다.

| 항목 | 값 |
|---|---|
| 지원 플랫폼 | linux-x64, linux-arm64, macos-arm64, windows-x64 (Windows ARM64·Intel Mac 미지원) |
| 패키지 저장소 | nuget.org, Maven Central, npm, vcpkg/Conan + GitHub Release |

## Framework 패키지

아래 탭은 언어별 설치 명령과 host 등록 코드다. 설치 뒤 첫 handler를 작성하고 실행하는
절차는 각 언어의 "설치와 첫 동작" 장이 다룬다.

=== "C#/.NET"

    ```bash
    dotnet add package Zlink                       # Core 메시징 엔진(.NET binding)
    dotnet add package Zlink.Framework             # 계약과 runtime
    dotnet add package Zlink.Framework.AspNetCore  # DI·hosted service 등록
    ```

    ```csharp
    builder.Services.AddZLinkFramework(options =>
    {
        // handler 타입을 찾는다.
        options.AddHandlersFromAssemblyOf<Program>();
        // 찾은 handler를 어느 channel에 열지는 따로 등록한다.
        options.AddRouteMesh("services").Listen("tcp://0.0.0.0:7101")
            .Channel("greeting").Server()
            .AddRequestHandler<GreetingHandler, Hello, Greeting>();
    });
    ```

    `AddZLinkFramework`가 framework host를 ASP.NET Core의 DI와 lifecycle에 등록한다.
    `AddHandlersFromAssemblyOf`는 handler 타입을 찾기만 한다. 어느 channel에 노출할지는
    `Channel(...).Server()`에 따로 등록한다.
    런타임은 .NET 8 이상이 필요하다. 다음 절차는
    [설치와 첫 동작](dotnet/guide/server/02-getting-started.ko.md)에 있다.

=== "C++"

    ```cmake
    find_package(zlink CONFIG REQUIRED)            # Core: vcpkg `zlink` 또는 Conan `zlink`
    find_package(zlink_framework CONFIG REQUIRED)  # Framework: vcpkg overlay port `zlink-framework`
    target_link_libraries(app PRIVATE zlink::framework)
    ```

    설치 경로는 셋이다. `zlink`는 아직 공식 vcpkg 레지스트리와 ConanCenter에 없으므로,
    앞의 둘은 이 저장소가 제공하는 overlay port와 recipe를 쓴다.

    ```bash
    git clone https://github.com/zlink-systems/zlink.git

    # vcpkg
    vcpkg install zlink zlink-cpp zlink-framework \
      --overlay-ports=zlink/vcpkg/ports --triplet=x64-linux

    # Conan
    conan create zlink/core/packaging/conan --version 1.1.0 --build=missing -s compiler.cppstd=gnu20
    conan create zlink/bindings/cpp/packaging/conan --build=missing -s compiler.cppstd=gnu20
    conan create zlink/framework/languages/cpp/packaging/conan --build=missing -s compiler.cppstd=gnu20
    ```

    셋째는 GitHub Release의 source archive를 `core/vX.Y.Z` → `cpp/vX.Y.Z` →
    `framework-cpp/vA.B.C` 순으로 빌드해 설치하는 것이다. 세 경로의 전체 절차는
    [C++ Quickstart](cpp/quickstart.ko.md)가 다룬다.

    IDE에서는 `CMakePresets.json`의 preset을 고른다. Visual Studio는 `vs2022`, Rider·VS Code·
    CLion은 `windows-ninja`·`linux-ninja`·`macos-ninja`다. 다음 절차는
    [설치와 첫 동작](cpp/guide/server/02-getting-started.ko.md)에 있다.

    **의존 패키지.** 설치 config는 어떤 서드파티 라이브러리도 함께 설치하지 않고
    `find_dependency`로 찾기만 한다. 소비자의 빌드에 이미 있는 라이브러리와 중복되지 않게 하고,
    버전을 소비자가 고르게 하기 위해서다.

    vcpkg overlay port `zlink-framework`로 설치하면 아래를 vcpkg가 함께 받으므로 따로 할 일이 없다.
    **source archive를 CMake로 직접 설치할 때는 아래를 먼저 갖춰야 한다.**

    | 패키지 | 쓰이는 곳 | 끄는 방법 |
    | --- | --- | --- |
    | `lz4` | STREAM 압축(`use_lz4()`) | `-DZLINK_FRAMEWORK_CPP_STREAM_WITH_LZ4=OFF -DZLINK_STREAM_CONNECTOR_WITH_LZ4=OFF` |
    | `openssl` | Stream Connector TLS | `-DZLINK_STREAM_CONNECTOR_WITH_TLS=OFF` |
    | `boost`(asio·beast) | HTTP·전송 | 끌 수 없다. Core와 framework가 **같은 Boost 트리**를 써야 한다 |
    | `nlohmann_json` | 기본 JSON serializer | 끌 수 없다 |
    | `opentelemetry-cpp` | 관측 | 끌 수 없다 |
    | `protobuf` | protobuf codec | |
    | `redis-plus-plus` | Redis location store | |

    압축 옵션은 둘 다 기본값이 `ON`이고, 켜져 있으면 `find_package(lz4 REQUIRED)`라 **없으면
    configure가 실패한다.** 압축을 쓰지 않는다면 위 표의 방법으로 끄면 `lz4` 없이 빌드된다.
    설치되는 `Findlz4.cmake`는 CMake config package를 먼저 찾고 없으면 시스템 설치본을 쓴다.

=== "Java"

    ```kotlin
    dependencies {
        implementation("systems.zlink:zlink-framework-core")                // 계약과 runtime
        implementation("systems.zlink:zlink-framework-spring-boot-starter") // DI·수명주기 등록
    }
    ```

    Spring Boot auto-configuration이 framework host를 Bean으로 등록하고 애플리케이션의
    lifecycle에 연결한다. 런타임은 JDK 25 이상이 필요하다. 다음 절차는
    [설치와 첫 동작](java/guide/server/02-getting-started.ko.md)에 있다.

=== "Kotlin"

    ```kotlin
    dependencies {
        implementation("systems.zlink:zlink-framework-core")
        implementation("systems.zlink:zlink-framework-spring-boot-starter")
        implementation("systems.zlink:zlink-framework-kotlin")              // coroutine idiom
    }
    ```

    Java와 같은 starter를 쓰고, `zlink-framework-kotlin`이 suspend 함수 기반의 handler 작성
    방식을 더한다. 런타임은 Java와 같은 JDK 25 이상이 필요하다. 다음 절차는
    [설치와 첫 동작](kotlin/guide/server/02-getting-started.ko.md)에 있다.

=== "Node/TypeScript"

    ```bash
    npm install @zlink-systems/framework   # 계약과 runtime (binding @zlink-systems/zlink 포함)
    npm install @zlink-systems/nestjs      # DI·모듈 등록
    ```

    `@zlink-systems/nestjs`는 NestJS의 module과 DI에 framework host를 등록한다. NestJS를
    쓰지 않는 서버(Express 등)는 framework 패키지만 설치하고 host를 코드에서 직접 시작한다.
    런타임은 Node.js 22 이상이 필요하다. 다음 절차는
    [설치와 첫 동작](node/guide/server/02-getting-started.ko.md)에 있다.

## Binding만 사용

framework 없이 Core API를 언어별 package로 직접 쓰려면 [Bindings 가이드](../../../bindings/doc/guide/README.ko.md)에서
언어를 고른다. 7개 언어(C++, .NET, Java, Node.js, Python, Go, Rust)의 설치 절차와 5분 예제가 있다.

## 저장소에서 빌드

Core를 소스에서 빌드하거나 저장소의 현재 source로 로컬 패키지를 만드는 절차는 저장소의
[빌드 가이드](../../../doc/building/build-guide.ko.md)와
[local package 가이드](../../../scripts/local-package/README.ko.md)가
소유한다. 패키지 사용자에게는 필요하지 않다.

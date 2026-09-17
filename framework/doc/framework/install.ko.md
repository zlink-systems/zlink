# 설치

## :material-server: Framework 패키지 { .zlink-band .band-server }

**C#/.NET · C++ · Java · Kotlin · Node/TypeScript** 다섯 언어를 지원한다. 다섯 모두 같은
channel·Spot·Actor 계약을 사용하므로, 한 mesh 안에서 언어가 달라도 서로 호출한다.

| 항목 | 값 |
|---|---|
| 지원 플랫폼 | linux-x64, linux-arm64, macos-arm64, windows-x64 (Windows ARM64·Intel Mac 미지원) |
| 패키지 저장소 | nuget.org, Maven Central, npm, vcpkg/Conan + GitHub Release |

**설치하는 것은 host 패키지 하나다.** 그 하나가 framework runtime과 Core 메시징 엔진을
포함한다. 각 탭에 그 포함 목록과 선택 패키지가 있다.

=== "C#/.NET"

    ```bash
    dotnet add package Zlink.Framework.AspNetCore
    ```

    | 포함 패키지 | 역할 |
    |---|---|
    | `Zlink.Framework` | framework 계약과 runtime |
    | `Zlink` | **Core 메시징 엔진의 `.NET` binding** |
    | `Zlink.Framework.Contracts` | codec 인터페이스와 framework 예외 |
    | `Zlink.Framework.Provider.Abstractions` | provider 확장점 |
    | `Zlink.Stream.Connector` | STREAM client 표면 |
    | `Zlink.HttpClient` | HTTP client |
    | `Microsoft.Extensions.DependencyInjection.Abstractions` · `.Logging.Abstractions` · `.Hosting.Abstractions` · `.Diagnostics.HealthChecks` | DI·로깅·host 수명주기·health check |

    | 선택 패키지 | 추가하는 경우 |
    |---|---|
    | `Zlink.Framework.Locations.Redis` | Redis location store를 사용할 때 |
    | `Zlink.Framework.Codecs.Protobuf` · `Zlink.Framework.Codecs.MessagePack` | 기본 JSON codec 대신 사용할 때 |

    ASP.NET Core host가 아니면 `Zlink.Framework`를 설치하고 host를 코드에서 직접 시작한다.
    런타임은 .NET 8 이상이 필요하며, 첫 실행까지의 절차는
    [퀵스타트](dotnet/quickstart.ko.md)가 다룬다.

=== "C++"

    ```cmake
    find_package(zlink_framework CONFIG REQUIRED)
    target_link_libraries(app PRIVATE zlink::framework)
    ```

    | 포함 패키지 | 역할 |
    |---|---|
    | `zlink::framework` | framework 계약과 runtime |
    | config `zlink` | **Core 메시징 엔진** — 버전 일치 조건으로 함께 찾는다 |
    | config `zlink_cpp` | Core의 C++ binding — 버전 일치 조건으로 함께 찾는다 |
    | `zlink::http_client` | HTTP client |
    | `zlink::stream_connector` · `zlink::stream_connector_codecs` | STREAM client 표면 |
    | `zlink::framework_provider_abstractions` | provider 확장점 |

    `find_package` 한 줄이 위를 모두 찾으므로 Core와 binding을 따로 적지 않는다.

    | 선택 타깃 | 추가하는 경우 |
    |---|---|
    | `zlink::framework_locations_redis` | Redis location store를 사용할 때 |
    | `zlink::framework_codec_protobuf` · `zlink::framework_codec_messagepack` | 기본 JSON codec 대신 사용할 때 |
    | `zlink::stream_connector_throwing` | 오류를 예외로 받을 때 |
    | `zlink::stream_e2e_client` | e2e·성능 시나리오 client를 작성할 때 |

    **소비자가 갖추는 서드파티.** 설치 config는 이들을 함께 설치하지 않고 `find_dependency`로
    찾기만 한다. 소비자의 빌드에 이미 있는 라이브러리와 중복되지 않게 하고, 버전을 소비자가
    선택하게 하기 위해서다.

    | 패키지 | 쓰이는 곳 | 끄는 방법 |
    | --- | --- | --- |
    | `lz4` | STREAM 압축 | `-DZLINK_FRAMEWORK_CPP_STREAM_WITH_LZ4=OFF -DZLINK_STREAM_CONNECTOR_WITH_LZ4=OFF` |
    | `openssl` | Stream Connector TLS | `-DZLINK_STREAM_CONNECTOR_WITH_TLS=OFF` |
    | `boost`(asio·beast) | HTTP·전송 | 끌 수 없다. Core와 framework가 **같은 Boost 트리**를 사용해야 한다 |
    | `nlohmann_json` | 기본 JSON serializer | 끌 수 없다 |
    | `opentelemetry-cpp` | 관측 | 끌 수 없다 |
    | `protobuf` | protobuf codec | 해당 타깃을 링크할 때만 |
    | `redis-plus-plus` · `libuv` | Redis location store | 해당 타깃을 링크할 때만 |

    압축 옵션은 둘 다 기본값이 `ON`이고, 켜져 있으면 `find_package(lz4 REQUIRED)`라 **없으면
    configure가 실패한다.**

    **설치.** `zlink`는 아직 공식 vcpkg 레지스트리와 ConanCenter에 없으므로, 이 저장소가
    제공하는 overlay port와 recipe를 사용한다.

    ```bash
    git clone https://github.com/zlink-systems/zlink.git

    # vcpkg
    vcpkg install zlink zlink-cpp zlink-framework \
      --overlay-ports=zlink/vcpkg/ports --triplet=x64-linux

    # Conan
    conan create zlink/core/packaging/conan --build=missing -s compiler.cppstd=gnu20
    conan create zlink/bindings/cpp/packaging/conan --build=missing -s compiler.cppstd=gnu20
    conan create zlink/framework/languages/cpp/packaging/conan --build=missing -s compiler.cppstd=gnu20
    ```

    셋째 경로는 GitHub Release의 source archive를 `core/vX.Y.Z` → `cpp/vX.Y.Z` →
    `framework-cpp/vA.B.C` 순으로 빌드해 설치하는 것이다.

    IDE에서는 `CMakePresets.json`의 preset을 선택한다. Visual Studio는 `vs2022`, Rider·VS
    Code·CLion은 `windows-ninja`·`linux-ninja`·`macos-ninja`다. C++20 컴파일러가 필요하며,
    세 경로의 전체 절차는 [퀵스타트](cpp/quickstart.ko.md)가 다룬다.

=== "Java"

    ```kotlin
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.16.0")
    ```

    | 포함 패키지 | 역할 |
    |---|---|
    | `zlink-framework-core` | framework 계약과 runtime |
    | `systems.zlink:zlink` | **Core 메시징 엔진의 Java binding** |
    | `zlink-framework-provider-abstractions` | provider 확장점 |
    | `zlink-http-client` | HTTP client |
    | `io.netty:netty-buffer` · `com.fasterxml.jackson.core:jackson-databind` | 버퍼와 JSON |
    | `org.lz4:lz4-java` · `org.slf4j:slf4j-api` | 압축과 로깅 |
    | `spring-boot-autoconfigure` · `spring-context` · `spring-boot-actuator` | DI·수명주기·health check |

    | 선택 아티팩트 | 추가하는 경우 |
    |---|---|
    | `zlink-framework-locations-redis` | Redis location store를 사용할 때 |
    | `zlink-framework-codec-protobuf` · `zlink-framework-codec-msgpack` | 기본 JSON codec 대신 사용할 때 |
    | `zlink-stream-connector` | 서버가 STREAM client도 될 때 |
    | `zlink-framework-testkit` | 테스트에서 runtime을 띄울 때 |

    전부 `systems.zlink` group이다. 런타임은 JDK 25 이상이 필요하며, 첫 실행까지의 절차는
    [퀵스타트](java/quickstart.ko.md)가 다룬다.

=== "Kotlin"

    ```kotlin
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.16.0")
    implementation("systems.zlink:zlink-framework-kotlin:0.16.0")
    ```

    | 포함 패키지 | 역할 |
    |---|---|
    | `zlink-framework-core` | framework 계약과 runtime |
    | `systems.zlink:zlink` | **Core 메시징 엔진의 Java binding** |
    | `zlink-framework-provider-abstractions` | provider 확장점 |
    | `zlink-http-client` | HTTP client |
    | `zlink-stream-connector` | STREAM client 표면 |
    | `io.netty:netty-buffer` · `com.fasterxml.jackson.core:jackson-databind` | 버퍼와 JSON |
    | `org.lz4:lz4-java` · `org.slf4j:slf4j-api` | 압축과 로깅 |
    | `spring-boot-autoconfigure` · `spring-context` · `spring-boot-actuator` | DI·수명주기·health check |
    | `kotlinx-coroutines-core` · `kotlinx-coroutines-jdk8` | suspend 함수 경계 |
    | `jackson-module-kotlin` | Kotlin data class를 JSON으로 다룬다 |

    | 선택 아티팩트 | 추가하는 경우 |
    |---|---|
    | `zlink-framework-locations-redis` | Redis location store를 사용할 때 |
    | `zlink-framework-codec-protobuf` · `zlink-framework-codec-msgpack` | 기본 JSON codec 대신 사용할 때 |
    | `zlink-framework-testkit` | 테스트에서 runtime을 띄울 때 |
    | `zlink-http-client-kotlin` | HTTP client를 coroutine으로 호출할 때 |

    선택 아티팩트는 `zlink-http-client-kotlin`을 빼면 Java와 같다. 런타임도 Java와 같은 JDK 25
    이상이 필요하며, 첫 실행까지의 절차는 [퀵스타트](kotlin/quickstart.ko.md)가 다룬다.

=== "Node/TypeScript"

    ```bash
    npm install @zlink-systems/nestjs
    ```

    | 포함 패키지 | 역할 |
    |---|---|
    | `@zlink-systems/framework` | framework 계약과 runtime |
    | `@zlink-systems/zlink` | **Core 메시징 엔진의 Node binding** |
    | `@zlink-systems/stream-wire` | STREAM wire 계층 |
    | `@zlink-systems/http-client` | HTTP client |
    | `@opentelemetry/api` · `@opentelemetry/api-logs` | 관측 |
    | `@nestjs/common` · `reflect-metadata` · `rxjs` | DI·모듈 등록 |

    | 선택 패키지 | 추가하는 경우 |
    |---|---|
    | `@zlink-systems/framework-locations-redis` | Redis location store를 사용할 때 |
    | `@zlink-systems/framework-codec-protobuf` · `@zlink-systems/framework-codec-msgpack` | 기본 JSON codec 대신 사용할 때 |
    | `@zlink-systems/stream-connector` | 서버가 STREAM client도 될 때 |

    NestJS를 사용하지 않는 서버(Express 등)는 `@zlink-systems/framework`를 설치하고 host를
    코드에서 직접 시작한다. 런타임은 Node.js 22 이상이 필요하며, 첫 실행까지의 절차는
    [퀵스타트](node/quickstart.ko.md)가 다룬다.

## :material-gamepad-variant: Client stream connector { .zlink-band .band-client }

어느 connector를 설치하는지는 언어가 아니라 **엔진과 빌드 타깃**이 정한다. 규칙 하나로
줄이면 — **웹(브라우저·WASM)으로 빌드하는 순간 언어와 무관하게 TypeScript connector를
사용한다.** 브라우저 샌드박스에서는 어느 언어도 OS 소켓을 열지 못하기 때문이다.

=== "Unity"

    | 빌드 | connector | 설치 |
    |---|---|---|
    | 네이티브 | `.NET` | `dotnet add package Zlink.Stream.Connector` |
    | WebGL | TypeScript | `com.zlink.stream-connector.webgl` UPM 패키지 |

    네이티브 빌드는 `.NET` connector 패키지를 그대로 사용한다. Unity 전용 패키지는 없다.

    WebGL 빌드는 Package Manager의 **Add package from git URL**에 아래를 넣어 설치한다.
    태그를 고정하지 않으면 browser bundle과 서버 버전이 어긋난다.

    ```
    https://github.com/zlink-systems/zlink.git?path=framework/languages/unity/com.zlink.stream-connector.webgl#framework-node/v0.16.0
    ```

    UPM 어댑터는 npm 패키지의 browser bundle을 담고 jslib·C# 호출 경계만 제공한다. **C# 표면은
    네이티브 패키지와 같다.** 두 빌드의 절차는
    [Unity 네이티브](dotnet/guide/stream-connector/02-unity.ko.md)와
    [Unity WebGL](node/guide/stream-connector/03-unity-webgl.ko.md)이 다룬다.

=== "Unreal"

    C++ connector의 Unreal plugin을 source로 편입한다. 웹 빌드 대상이 아니다.

    | 산출물 | 배포 |
    |---|---|
    | `zlink-unreal-stream-connector` | source plugin |

    plugin은 `zlink::stream_connector`와 `zlink::stream_connector_codecs` 위에 구성되는
    어댑터이고, C++ connector를 빌드하면 함께 나온다. 절차는
    [engine adapter 가이드](cpp/guide/stream-connector/09-engine-adapters.ko.md)가 다룬다.

=== "Godot"

    | 빌드 | connector | 산출물 |
    |---|---|---|
    | GDExtension(C++) | C++ | `zlink-godot-stream-connector` source GDExtension |
    | Godot C# | `.NET` | `Zlink.Stream.Connector` (NuGet) |
    | Godot Web | TypeScript | `@zlink-systems/stream-connector` (npm) |

    GDExtension은 C++ connector를 빌드하면 함께 나온다. Godot C#은 `.NET` connector 패키지를 그대로 사용한다. 절차는
    [Godot C#](dotnet/guide/stream-connector/03-godot-csharp.ko.md)과
    [engine adapter 가이드](cpp/guide/stream-connector/09-engine-adapters.ko.md)가 다룬다.

=== "Cocos"

    | 빌드 | connector | 산출물 |
    |---|---|---|
    | Axmol(네이티브) | C++ | `zlink-axmol-connector` source package |
    | Cocos Creator web | TypeScript | `@zlink-systems/stream-connector` (npm) |

    Axmol 어댑터는 C++ connector를 빌드하면 함께 나온다. 절차는
    [engine adapter 가이드](cpp/guide/stream-connector/09-engine-adapters.ko.md)가 다룬다.

=== "웹"

    브라우저 web client와 WASM 빌드가 대상이다. Cocos Creator web·Godot Web·Unity WebGL도
    같은 package root를 사용한다.

    ```bash
    npm install @zlink-systems/stream-connector
    ```

    | 패키지 | 역할 |
    |---|---|
    | `@zlink-systems/stream-connector` | connector 본체. **필수** |
    | `@zlink-systems/stream-wire` | wire 계층. connector가 **포함한다** |
    | `@zlink-systems/framework-codec-msgpack` | 기본 JSON codec 대신 사용할 때. browser 진입점을 따로 낸다 |

    **브라우저 계열은 `ws`·`wss`만 사용한다.** `tcp://`·`tls://` endpoint를 받으면 구성 오류로
    즉시 실패한다. 절차는
    [브라우저 가이드](node/guide/stream-connector/02-browser.ko.md)가 다룬다.

=== "e2e·도구"

    엔진 없이 접속하는 client다. 시나리오 테스트, 성능 측정, 운영 도구가 여기에 해당한다.

    | 언어 | 패키지 | 저장소 |
    |---|---|---|
    | `.NET` | `Zlink.Stream.Connector` | nuget.org |
    | C++ | `zlink::stream_e2e_client` — connector에 시나리오 helper를 추가한 타깃 | vcpkg · Conan · source |
    | Java · Kotlin | `systems.zlink:zlink-stream-connector:0.16.0` | Maven Central |
    | Node.js | `@zlink-systems/stream-connector` | npm |

    C++만 전용 타깃을 둔다. `zlink::stream_e2e_client`는 `zlink::stream_connector`에 시나리오
    작성용 helper를 추가한 타깃이고, 나머지 언어는 connector 패키지 하나로 같은 일을 한다.

    Java 모듈은 서버 framework와 독립이라 Spring Boot가 없는 client에서도 사용한다. Kotlin에서
    coroutine 표면이 필요하면 `zlink-framework-kotlin`을 추가하는데, 이 아티팩트는
    `zlink-framework-core`도 포함한다.

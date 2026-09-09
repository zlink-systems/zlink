# 설치

이 문서는 ZLink를 애플리케이션에 넣는 첫 단계다. framework는 각 언어의 패키지 저장소에 게시된
패키지로 설치한다. framework 패키지는 자신이 의존하는 binding 패키지를 함께 끌어오고, binding
패키지에는 Core 엔진의 native 런타임이 들어 있으므로 Core를 직접 빌드할 필요가 없다.

| 항목 | 값 |
|---|---|
| 게시 버전 | framework 0.11, binding 0.17.6 |
| 지원 플랫폼 | linux-x64, linux-arm64, macos-arm64, windows-x64, windows-arm64 (Intel Mac 미지원) |
| 패키지 저장소 | nuget.org, Maven Central, npm, vcpkg/Conan + GitHub Release |

## Framework 패키지

아래 탭은 언어별 설치 명령과 host 등록 코드다. 설치 뒤 첫 handler를 작성하고 실행하는
절차는 각 언어의 "설치와 첫 동작" 장이 다룬다.

framework는 각 언어의 패키지 저장소에 게시된 패키지로 설치한다. framework 패키지는
자신이 의존하는 binding 패키지를 함께 끌어오고, binding 패키지에는 Core 엔진의 native
런타임이 들어 있으므로 Core를 직접 빌드할 필요가 없다. 현재 게시 버전은 framework 0.11,
binding 0.17.6이다. 지원 플랫폼은 linux-x64, linux-arm64, macos-arm64, windows-x64,
windows-arm64다.

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
        options.AddHandlersFromAssemblyOf<Program>();
        options.AddRouteMesh("services").Listen("tcp://0.0.0.0:7101")
            .Channel("greeting").Server();
    });
    ```

    `AddZLinkFramework`가 framework host를 ASP.NET Core의 DI와 lifecycle에 등록한다.
    다음 절차는 [설치와 첫 동작](dotnet/guide/server/02-getting-started.ko.md)에 있다.

=== "C++"

    ```cmake
    find_package(zlink CONFIG REQUIRED)            # Core: vcpkg `zlink` 또는 Conan `zlink`
    find_package(zlink_framework CONFIG REQUIRED)  # Framework: vcpkg overlay port `zlink-framework`
    target_link_libraries(app PRIVATE zlink::framework)
    ```

    Core는 vcpkg 또는 Conan 레시피로 설치한다. framework는 GitHub Release의 source archive를
    CMake로 설치하거나, 저장소가 제공하는 vcpkg overlay port `zlink-framework`로 설치한다.
    IDE에서는 `CMakePresets.json`의 preset을 고른다. Visual Studio는 `vs2022`, Rider·VS Code·
    CLion은 `windows-ninja`·`linux-ninja`·`macos-ninja`다. 다음 절차는
    [설치와 첫 동작](cpp/guide/server/02-getting-started.ko.md)에 있다.

=== "Java"

    ```kotlin
    dependencies {
        implementation("systems.zlink:zlink-framework-core")                // 계약과 runtime
        implementation("systems.zlink:zlink-framework-spring-boot-starter") // DI·수명주기 등록
    }
    ```

    Spring Boot auto-configuration이 framework host를 Bean으로 등록하고 애플리케이션의
    lifecycle에 연결한다. 다음 절차는 [설치와 첫 동작](java/guide/server/02-getting-started.ko.md)에
    있다.

=== "Kotlin"

    ```kotlin
    dependencies {
        implementation("systems.zlink:zlink-framework-core")
        implementation("systems.zlink:zlink-framework-spring-boot-starter")
        implementation("systems.zlink:zlink-framework-kotlin")              // coroutine idiom
    }
    ```

    Java와 같은 starter를 쓰고, `zlink-framework-kotlin`이 suspend 함수 기반의 handler 작성
    방식을 더한다. 다음 절차는 [설치와 첫 동작](kotlin/guide/server/02-getting-started.ko.md)에
    있다.

=== "Node/TypeScript"

    ```bash
    npm install @zlink-systems/framework   # 계약과 runtime (binding @zlink-systems/zlink 포함)
    npm install @zlink-systems/nestjs      # DI·모듈 등록
    ```

    `@zlink-systems/nestjs`는 NestJS의 module과 DI에 framework host를 등록한다. NestJS를
    쓰지 않는 서버(Express 등)는 framework 패키지만 설치하고 host를 코드에서 직접 시작한다.
    다음 절차는 [설치와 첫 동작](node/guide/server/02-getting-started.ko.md)에 있다.

## Binding만 사용

framework 없이 Core API를 언어별 package로 직접 쓰려면 [Bindings 가이드](bindings/guide/README.ko.md)에서
언어를 고른다. 7개 언어(C, C++, .NET, Java, Node.js, Python, Go, Rust)의 설치 절차와 5분 예제가 있다.

## 저장소에서 빌드

Core를 소스에서 빌드하거나 저장소의 현재 source로 로컬 패키지를 만드는 절차는 저장소의
[빌드 가이드](https://github.com/zlink-systems/zlink/blob/main/doc/building/build-guide.ko.md)와
[local package 가이드](https://github.com/zlink-systems/zlink/blob/main/scripts/local-package/README.ko.md)가
소유한다. 패키지 사용자에게는 필요하지 않다.

[English](./README.md) | **한국어**

# zlink

> 메시징 엔진, 7개 언어 bindings, 실시간 메시징 framework —
> 연결·상태·라우팅을 함께 관리해야 하는 서비스를 위한 플랫폼입니다.

[![Build](https://github.com/zlink-systems/zlink/actions/workflows/build.yml/badge.svg)](https://github.com/zlink-systems/zlink/actions/workflows/build.yml)
[![License: MPL-2.0 / FSL-1.1 / Apache-2.0](https://img.shields.io/badge/License-multiple-blue.svg)](./doc/license/README.ko.md)

| | |
|---|---|
| **공식 사이트** | [zlink.systems](https://zlink.systems/ko/) |
| **가이드와 API 레퍼런스** | [zlink.systems](https://zlink.systems/ko/) |
| **설치** | [zlink.systems/ko/install](https://zlink.systems/ko/install/) |

제품 문서 — 가이드, 튜토리얼, API 레퍼런스 — 는 공식 사이트가 소유합니다. 이
페이지는 저장소를 설명합니다. 무엇이 들어 있고, 어떻게 계층화되어 있으며, 소스에서
어떻게 빌드하는지를 다룹니다.

## zlink란

zlink는 세 계층으로 구성된 메시징 플랫폼입니다. 각 계층은 단독으로 쓸 수 있고,
아래 계층 위에 한 단계씩 추상화를 더합니다.

| 계층 | 역할 | 언어 |
|---|---|---|
| [`core/`](./core/) | Boost.Asio 기반 네이티브 메시징 엔진과 공개 C API | C |
| [`bindings/`](./bindings/) | Core를 각 언어의 API와 리소스 수명 모델로 제공 | C++, .NET, Java, Node.js, Python, Go, Rust |
| [`framework/`](./framework/) | typed handler, 라우팅, 상태 소유 단위, location runtime | C++, .NET, JVM(Java/Kotlin), Node.js |

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="./doc/assets/layer-stack-dark.svg">
  <img alt="비즈니스 로직과 ZLink Framework는 application host 안에서 동작하고, 공개 API 아래에는 언어별 bindings와 네이티브 zlink Core C API, 전송 계층이 있습니다." src="./doc/assets/layer-stack-light.svg" width="900">
</picture>

**Core**는 [libzmq](https://github.com/zeromq/libzmq) v4.3.5에서 출발해 Boost.Asio와
핵심 메시징 패턴에 집중하도록 재구성한 네이티브 메시징 엔진입니다. PAIR, PUB/SUB,
XPUB/XSUB, DEALER/ROUTER, STREAM 소켓을 `tcp`, `ipc`, `inproc`, `tls`, `ws`, `wss`
전송 위에서 제공하며 routing ID, socket monitoring, backpressure를 포함합니다.
소켓과 전송을 직접 조합하려면 여기서 시작합니다.

**Framework**는 Binding 위에 올라가 application host의 lifecycle과 DI에 메시징
계층을 연결합니다. Spring 위에 Spring MVC가 웹 계층으로 올라가듯, ZLink Framework는
ASP.NET Core, Spring Boot와 NestJS에 실시간 메시징 계층으로 통합됩니다. C++에서는
zlink framework host가 DI, 설정, HTTP hosting과 프로세스 lifecycle을 함께 제공합니다.
소켓을 직접 엮는 대신 분산 실시간 서비스를 만들려면 여기서 시작합니다.

## 왜 zlink인가

실시간 서비스는 연결 관리, 상태 소유, 라우팅을 동시에 풀어야 하고 이 셋은 계속
충돌합니다. room은 이 프로세스에 있고 플레이어의 session은 저 프로세스에 있으며,
요청은 지금 그 상태를 소유한 노드에 도달해야 하고, 그 전부가 재연결과 rolling
restart를 견뎌야 합니다. 대부분의 스택은 이것을 애플리케이션 코드에 맡깁니다.

Framework는 이 조각들을 런타임의 일급 개념으로 제공합니다. 애플리케이션은 typed
handler와 client를 작성하고, Framework는 transport 연결, peer discovery, 위치 조회,
라우팅, 재연결, packet codec과 reply correlation을 관리합니다.

| 기능 | 용도 |
|---|---|
| **Channel / RouteMesh** | 논리 ChannelName으로 서버를 찾고 서버 간 요청, 응답, command와 event를 전달 |
| **Spot** | room, stage, zone처럼 상태를 소유하는 단위를 직렬 실행 문맥에서 처리 |
| **Actor** | 연결이나 사용자를 나타내는 상태 객체의 lifecycle, session binding과 위치 이동 처리 |
| **STREAM** | TCP/TLS/WS/WSS 외부 client 연결의 lifecycle, framing과 packet dispatch 관리 |
| **Location runtime** | 서비스, Spot과 Actor의 현재 위치를 발견하고 연결 상태를 유지 |
| **Graceful drain** | 신규 작업을 제한하고 진행 중인 작업과 상태 이동을 고려해 종료 |

실시간 게임 서버, 장기 연결을 유지하는 stateful 서비스, 여러 언어로 구성된 분산
서비스에 적합합니다. room, zone, match와 actor 기반 토폴로지는 동일한 RouteMesh,
Spot, Actor와 STREAM 조합으로 구성합니다.

## 코드로 보기

`greeting` 채널을 소유하는 서버 — 등록, 그리고 handler입니다.

```csharp
// Server — handler를 등록하고 "greeting"을 담당한다고 알린다.
builder.Services.AddZLinkFramework(options =>
{
    options.AddHandlersFromAssemblyOf<Program>();

    var mesh = options.AddRouteMesh("services").Listen("tcp://0.0.0.0:7101");
    mesh.Channel("greeting").Server();
});

public sealed class HelloHandler : IZLinkRequestHandler<Hello, Greeting>
{
    public ValueTask<Greeting> HandleAsync(
        Hello request, IZLinkMessageContext context, CancellationToken cancellationToken)
        => ValueTask.FromResult(new Greeting($"hello, {request.Name}"));
}
```

client는 노드를 지정하지 않고 `greeting`을 호출하며, 어느 프로세스가 처리할지는
런타임에 결정됩니다. 같은 등록·handler 구조가 C++, Java, Kotlin, TypeScript에도
있습니다. [설치와 첫 실행](https://zlink.systems/ko/dotnet/guide/server/02-getting-started/)에서
장 상단의 언어 탭을 바꿔 확인하세요.

## 성능

Framework 언어 전반에 걸쳐 messaging 처리량과 지연을 gRPC와 비교 측정하며, 측정
규격과 언어별 결과를 함께 공개합니다:
[gRPC 비교 보고서](https://zlink.systems/ko/bench/comparison/).

## 언어 지원

**ZLink Framework — 4개 런타임.** 각 런타임은 host 언어로 독립 구현됩니다. 네이티브
service runtime도, service C ABI도 공유하지 않습니다. 공유하는 것은 공개 계약,
버전이 있는 wire protocol, 공통 검증 fixture뿐이며, 그래서 .NET 서비스와 Java
서비스가 같은 mesh 위에서 서로 통신합니다.

| 런타임 | Application host | 설치 |
|---|---|---|
| .NET / C# | ASP.NET Core | `dotnet add package Zlink.Framework.AspNetCore` |
| JVM — Java, Kotlin | Spring Boot | `systems.zlink:zlink-framework-spring-boot-starter` |
| Node.js — TypeScript, JavaScript | NestJS | `npm install @zlink-systems/framework @zlink-systems/nestjs` |
| C++ | zlink framework host | vcpkg port `zlink-framework` |

**Bindings — 7개 언어.** Framework 없이 Core 소켓과 전송을 직접 조합할 때
사용합니다. 각 패키지는 플랫폼 네이티브 Core를 함께 담고 있어 이 저장소를 빌드할
필요가 없습니다.

| 언어 | 설치 |
|---|---|
| C++ | vcpkg / Conan 패키지 `zlink` |
| .NET / C# | `dotnet add package Zlink` |
| Java, Kotlin | `systems.zlink:zlink` |
| Node.js, JavaScript | `npm install @zlink-systems/zlink` |
| Python | `pip install zlink` |
| Go | `go get zlink.systems/zlink` |
| Rust | `cargo add zlink` |

C는 별도 Binding이 아니라 공개 Core API입니다. 각 Binding은 PAIR, PUB/SUB,
DEALER/ROUTER, request/reply, STREAM, monitoring 샘플을 함께 제공합니다.

설치 절차와 언어별 5분 예제는 [설치](https://zlink.systems/ko/install/) 페이지와
[Bindings 가이드](https://zlink.systems/ko/bindings/guide/)에 있습니다. 정식 계약은
[Core 스펙](https://zlink.systems/ko/spec/)과
[Framework 공통 스펙](https://zlink.systems/ko/common/spec/server/)입니다.

## 소스에서 빌드

저장소에서 빌드하는 것은 zlink 자체를 작업하거나, 릴리스되지 않은 리비전에 링크할
때를 위한 것입니다.

**요구 사항** — CMake 3.10+, C++17 컴파일러(GCC 7+, Clang 5+, MSVC 2017+), TLS/WSS를
켤 때 OpenSSL. Framework 런타임은 host 언어의 툴체인이 추가로 필요합니다.

Core는 Linux에 x64/ARM64, macOS에 ARM64, Windows에 x64 빌드 경로를 제공합니다. Binding과
Framework 구현의 런타임 지원, 패키지 형식, 플랫폼별 제약은 각 언어 문서를
참고하세요.

[기여·운영 핸드북](./CONTRIBUTING.ko.md#1-5분-시작)의 5분 시작부터 보세요. clone,
테스트를 포함한 Core 빌드, Binding 스모크 테스트까지 이어집니다. 상세 절차는
계층별로 나뉘어 있습니다.

| 계층 | 빌드 문서 |
|---|---|
| Core | [빌드 가이드](./doc/building/build-guide.ko.md) · [CMake 옵션](./doc/building/cmake-options.ko.md) |
| Bindings | [로컬 Core로 Binding 링크](./doc/building/local-core-bindings.ko.md) · [로컬 패키지 러너](./scripts/local-package/README.ko.md) |
| Framework | [Framework 워크스페이스 구성](./doc/building/framework-workspace.ko.md), 이후 언어별 소스 루트 [C++](./framework/languages/cpp/), [.NET](./framework/languages/dotnet/), [JVM](./framework/languages/java/), [Node.js](./framework/languages/node/) |
| 패키징과 릴리스 | [패키징 가이드](./doc/building/packaging.ko.md) · [빌드·배포 파이프라인](./doc/building/release-pipeline.ko.md) |

Core 빌드 성공, 언어 패키지 빌드, clean consumer 검증, 실제 client/server 샘플
실행은 서로 다른 검증 단계입니다. 배포 전에는 사용하는 계층과 언어의 빌드, 테스트,
샘플 절차를 각각 수행하세요.

## 저장소 구성

| 경로 | 내용 |
|---|---|
| [`core/`](./core/) | Core 엔진, 공개 C API(`core/include`), Core 테스트 |
| [`bindings/`](./bindings/) | 7개 언어 Binding. 각 언어 샘플은 `bindings/<lang>/samples` |
| [`framework/`](./framework/) | `framework/languages/` 아래 4개 Framework 런타임과 언어별 샘플 |
| [`doc/`](./doc/) | 저장소 문서 — [색인](./doc/README.ko.md), 설계 원칙, 빌드와 릴리스 |
| [`scripts/`](./scripts/) | 로컬 패키징, gate, 성능 측정 도구 |

Framework 샘플은 개별 API 호출이 아니라 완결된 애플리케이션 시나리오를 검증하기
위해 client와 여러 서버 역할을 함께 실행합니다. 언어 구현을 고르기 전에
[공통 샘플 계약](./framework/doc/framework/common/sample/README.ko.md)을 먼저 읽으세요.

## 기여

[기여·운영 핸드북](./CONTRIBUTING.ko.md)을 먼저 읽어주세요. 빌드, 코드와 문서 규칙,
커밋 전 gate, 브랜치·커밋·PR·릴리스 절차를 다룹니다. 이슈와 PR을 환영합니다.

## 라이선스

저장소 계층마다 라이선스가 다릅니다.

| 범위 | 라이선스 |
|---|---|
| `core/`, `bindings/` | [Mozilla Public License 2.0](./LICENSE) |
| `framework/` | [Functional Source License 1.1, ALv2 Future License](./framework/LICENSE) |
| 언어별 Framework `http-client` 패키지 | Apache License 2.0 |

상세 조건, 2년 후 Apache License 2.0 전환 정책, 재배포 고지는
[라이선스 가이드](./doc/license/README.ko.md)와
[THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md)를 참고하세요.

[libzmq](https://github.com/zeromq/libzmq) 기반 — Copyright (c) 2007-2024
Contributors, [`core/AUTHORS`](./core/AUTHORS) 참조.

[English](./README.md) | **한국어**

# zlink

> Core 메시징 엔진, 7개 언어 Bindings, 4개 언어 계열의 독립 런타임으로 구현된
> 실시간 메시징 Framework를 함께 제공하는 멀티언어 메시징 플랫폼입니다.

[![Build](https://github.com/zlink-systems/zlink/actions/workflows/build.yml/badge.svg)](https://github.com/zlink-systems/zlink/actions/workflows/build.yml)
[![License: MPL-2.0 / FSL-1.1 / Apache-2.0](https://img.shields.io/badge/License-multiple-blue.svg)](./doc/license/README.ko.md)

[공식 사이트](https://zlink-systems.github.io/zlink/ko/) ·
[전체 문서](./doc/README.ko.md) ·
[Core 가이드](./core/doc/guide/01-overview.ko.md) ·
[Bindings 가이드](./bindings/doc/guide/README.ko.md) ·
[Framework 가이드](./framework/doc/framework/common/guide/server/01-overview.ko.md) ·
[빌드 가이드](./doc/building/build-guide.ko.md)

## 한눈에 보기

zlink 저장소는 역할과 추상화 수준이 다른 세 계층으로 구성됩니다.

| 계층 | 역할 | 주요 대상 |
|---|---|---|
| [`core/`](./core/) | Boost.Asio 기반 네이티브 메시징 엔진과 C API | 저수준 메시징, 네이티브 통합 |
| [`bindings/`](./bindings/) | Core를 각 언어에 맞는 API와 리소스 수명 모델로 제공 | C++, .NET, Java, Node.js, Python, Go, Rust |
| [`framework/`](./framework/) | typed handler, Channel, RouteMesh, Spot, Actor, STREAM과 location runtime을 제공 | C++, .NET, JVM(Java/Kotlin), Node.js |

```text
Application
    │
ZLink Framework
  Channel · RouteMesh · Spot · Actor · STREAM
    │
Language Binding
    │
zlink Core
  PAIR · PUB/SUB · XPUB/XSUB · DEALER/ROUTER · STREAM
    │
tcp · ipc · inproc · tls · ws · wss
```

소켓과 전송을 직접 조합하려면 Core 또는 Binding에서 시작하고, 애플리케이션
host와 DI 안에서 분산 실시간 서비스를 구성하려면 Framework에서 시작합니다.

## 언어 지원

### Bindings: 7개 언어

zlink Core는 C API를 직접 제공합니다. 그 위에 다음 7개 언어 Binding을
제공합니다.

| 언어 | Binding 문서 | 소스 |
|---|---|---|
| C++ | [가이드](./bindings/doc/guide/cpp/index.ko.md) | [`bindings/cpp`](./bindings/cpp/) |
| .NET/C# | [가이드](./bindings/doc/guide/dotnet/index.ko.md) | [`bindings/dotnet`](./bindings/dotnet/) |
| Java | [가이드](./bindings/doc/guide/java/index.ko.md) | [`bindings/java`](./bindings/java/) |
| Node.js/TypeScript | [가이드](./bindings/doc/guide/node/index.ko.md) | [`bindings/node`](./bindings/node/) |
| Python | [가이드](./bindings/doc/guide/python/index.ko.md) | [`bindings/python`](./bindings/python/) |
| Go | [가이드](./bindings/doc/guide/go/index.ko.md) | [`bindings/go`](./bindings/go/) |
| Rust | [가이드](./bindings/doc/guide/rust/index.ko.md) | [`bindings/rust`](./bindings/rust/) |

- C는 별도 Binding이 아니라 Core의 기본 public API입니다.
- Kotlin은 Java Binding을 공유합니다.
- JavaScript는 Node.js Binding을 공유합니다.

### Framework: 4개 언어 계열, 4개 런타임 구현

ZLink Framework의 service runtime은 각 언어에서 독립적으로 구현되며, 공통
native service runtime이나 service C ABI를 공유하지 않습니다. 런타임 사이에는
공개 계약, versioned wire protocol과 검증 fixture가 공유됩니다.

| Framework 런타임 | 애플리케이션 통합 | 문서 | 소스 |
|---|---|---|---|
| C++ | zlink framework host | [C++ 문서](./framework/doc/framework/cpp/README.ko.md) | [`framework/languages/cpp`](./framework/languages/cpp/) |
| .NET/C# | ASP.NET Core | [.NET 문서](./framework/doc/framework/dotnet/README.ko.md) | [`framework/languages/dotnet`](./framework/languages/dotnet/) |
| JVM | Java/Kotlin, Spring Boot | [Java 문서](./framework/doc/framework/java/README.ko.md) · [Kotlin 문서](./framework/doc/framework/kotlin/README.ko.md) | [`framework/languages/java`](./framework/languages/java/) |
| Node.js | TypeScript/JavaScript, NestJS | [Node.js 문서](./framework/doc/framework/node/README.ko.md) | [`framework/languages/node`](./framework/languages/node/) |

따라서 Framework는 **4개 독립 런타임 구현**을 제공하며, JVM 구현은 Java와
Kotlin을, Node.js 구현은 TypeScript와 JavaScript를 함께 지원합니다.

## zlink Core

Core는 [libzmq](https://github.com/zeromq/libzmq) v4.3.5에서 출발해 핵심
메시징 패턴에 집중하도록 재구성한 네이티브 메시징 엔진입니다.

- PAIR, PUB/SUB, XPUB/XSUB, DEALER/ROUTER, STREAM 소켓
- Boost.Asio 기반 비동기 I/O
- `tcp`, `ipc`, `inproc`, `tls`, `ws`, `wss` 전송
- OpenSSL 기반 TLS와 WebSocket 통합
- routing ID, socket monitoring과 backpressure
- C API와 7개 언어 Binding에서 공유하는 메시징 의미

Core를 처음 사용한다면 [Core 개요](./core/doc/guide/01-overview.ko.md),
[소켓 패턴 선택 가이드](./core/doc/guide/03-0-socket-patterns.ko.md),
[Core 스펙](./core/doc/spec/README.ko.md) 순서로 읽는 것을 권장합니다.

## ZLink Framework

ZLink Framework는 application host의 lifecycle과 DI에 실시간 메시징 계층을
연결합니다. Spring 위에 Spring MVC가 웹 계층으로 올라가듯, ZLink Framework는
ASP.NET Core, Spring Boot와 NestJS에 실시간 메시징 계층으로 통합됩니다. C++에서는
framework host가 DI, 설정, HTTP hosting과 프로세스 lifecycle을 함께 제공합니다.

애플리케이션은 typed handler와 client를 작성하고, Framework는 transport 연결,
peer discovery, 위치 조회, 라우팅, 재연결, packet codec과 reply correlation을
관리합니다.

| 기능 | 용도 |
|---|---|
| **Channel / RouteMesh** | 논리 ChannelName으로 서버를 찾고 서버 간 요청, 응답, command와 event를 전달 |
| **Spot** | room, stage, zone처럼 상태를 소유하는 단위를 직렬 실행 문맥에서 처리 |
| **Actor** | 연결이나 사용자를 나타내는 상태 객체의 lifecycle, session binding과 위치 이동 처리 |
| **STREAM** | TCP/TLS/WS/WSS 외부 client 연결의 lifecycle, framing과 packet dispatch 관리 |
| **Location runtime** | 서비스, Spot과 Actor의 현재 위치를 발견하고 연결 상태를 유지 |
| **Graceful drain** | 신규 작업을 제한하고 진행 중인 작업과 상태 이동을 고려해 종료 |

Framework는 실시간 게임 서버, 장기 연결을 유지하는 stateful 서비스, 여러 언어로
구성된 분산 서비스처럼 연결·상태·라우팅을 함께 관리해야 하는 시스템에 적합합니다.
room, zone, match와 actor 기반 서비스 같은 토폴로지는 동일한 RouteMesh, Spot,
Actor와 STREAM 조합으로 구성할 수 있습니다.

더 자세한 설명은 [Framework 서버 개요](./framework/doc/framework/common/guide/server/01-overview.ko.md)를,
정식 의미와 책임 경계는 [Framework 공통 스펙](./framework/doc/framework/common/spec/README.ko.md)을
참고하세요.

## 빠른 시작

### 패키지 사용자

Binding이나 Framework package를 사용하는 애플리케이션은 Core 저장소를 먼저
빌드할 필요가 없습니다. .NET, Java, Node.js와 Go package 등은 platform native Core를
package에 포함하며, 다른 언어도 각 가이드가 설치와 native runtime 준비 방법을
소유합니다.

- Core API를 언어별 package로 사용하려면 [Bindings 언어 선택](./bindings/doc/guide/README.ko.md)에서
  사용할 언어의 설치 절차와 5분 예제를 확인하세요.
- ZLink Framework를 사용하려면 [Framework 시작하기](./framework/doc/framework/common/guide/server/02-getting-started.ko.md)에서
  C++, .NET, Java, Kotlin 또는 Node.js 탭을 선택하세요.
- 설치 후에는 해당 언어의 sample을 실행해 package와 native runtime이 실제
  client/server process에서 함께 동작하는지 확인하세요.

### 저장소에서 Core 빌드

다음 요구 사항과 명령은 zlink Core를 소스에서 빌드하는 개발자를 위한 것입니다.
언어별 package만 사용하는 애플리케이션의 공통 선행 요구 사항이 아닙니다.

#### 요구 사항

- CMake 3.10 이상
- C++17 지원 컴파일러
- TLS/WSS를 사용할 경우 OpenSSL

저장소 루트에서 다음 명령을 실행합니다.

```bash
cmake -S core -B core/build -DWITH_TLS=ON -DBUILD_TESTS=ON
cmake --build core/build
ctest --test-dir core/build --output-on-failure
```

플랫폼별 빌드 스크립트도 제공합니다.

```bash
# Linux
./core/builds/linux/build.sh x64 ON

# macOS
./core/builds/macos/build.sh arm64 ON

# Windows PowerShell
.\core\builds\windows\build.ps1 -Architecture x64 -RunTests "ON"
```

전체 옵션과 의존성은 [빌드 가이드](./doc/building/build-guide.ko.md)와
[CMake 옵션](./doc/building/cmake-options.ko.md)을 참고하세요.

### 저장소의 Core와 Bindings package 빌드

WSL 개발 환경에서 현재 source revision의 Core와 first-party Bindings package를
함께 만들려면 local package runner를 사용합니다.

```bash
# Core와 모든 first-party Binding package
scripts/local-package/build-wsl.sh

# 선택한 Binding package만 빌드
scripts/local-package/build-wsl.sh dotnet java node
```

출력 위치, package provenance와 언어별 산출물은
[local package 가이드](./scripts/local-package/README.ko.md)를 참고하세요. 이 runner는
Core와 Bindings package를 만들며 Framework 빌드 완료를 의미하지 않습니다.

Framework package 설치와 첫 실행은 [Framework 시작하기](./framework/doc/framework/common/guide/server/02-getting-started.ko.md)가
안내합니다. Framework source build와 test는 matching Binding package를 준비한 뒤
각 런타임의 source root에서 독립적으로 수행합니다:
[C++](./framework/languages/cpp/),
[.NET](./framework/languages/dotnet/),
[JVM](./framework/languages/java/),
[Node.js](./framework/languages/node/).

> Core 빌드 성공, 언어별 package 빌드, clean consumer 검증과 실제 client/server
> sample 실행은 서로 다른 검증 단계입니다. 배포 전에 사용하는 계층과 언어의
> 빌드·테스트·sample 절차를 각각 확인하세요.

## 샘플

### Core와 Bindings

각 Binding은 PAIR, PUB/SUB, DEALER/ROUTER, request/reply, STREAM과 monitoring의
기본 사용법을 보여 주는 언어별 sample을 제공합니다.

- [`bindings/cpp/samples`](./bindings/cpp/samples/)
- [`bindings/dotnet/samples`](./bindings/dotnet/samples/)
- [`bindings/java/samples`](./bindings/java/samples/)
- [`bindings/node/samples`](./bindings/node/samples/)
- [`bindings/python/samples`](./bindings/python/samples/)
- [`bindings/go/samples`](./bindings/go/samples/)
- [`bindings/rust/samples`](./bindings/rust/samples/)

### Framework

Framework sample은 단순 API 호출뿐 아니라 여러 역할의 server와 client를 실행해
실제 업무 흐름을 검증합니다. 공통 sample 계약을 먼저 확인한 뒤 언어별 구현을
선택하세요.

- [Framework 공통 sample](./framework/doc/framework/common/sample/README.ko.md)
- [`framework/languages/cpp/samples`](./framework/languages/cpp/samples/)
- [`framework/languages/dotnet/samples`](./framework/languages/dotnet/samples/)
- [`framework/languages/java/samples`](./framework/languages/java/samples/)
- [`framework/languages/node/samples`](./framework/languages/node/samples/)

## 문서 찾아가기

| 목적 | 문서 |
|---|---|
| 전체 문서 구조 확인 | [문서 네비게이션](./doc/README.ko.md) |
| Core 사용법 | [Core 사용자 가이드](./core/doc/guide/01-overview.ko.md) |
| Core 정식 계약 | [Core 스펙](./core/doc/spec/README.ko.md) |
| 언어별 Binding 사용법 | [Bindings 가이드](./bindings/doc/guide/README.ko.md) |
| Binding 정식 계약 | [Bindings 스펙](./bindings/doc/spec/README.ko.md) |
| Framework 개념과 사용 상황 | [Framework 서버 개요](./framework/doc/framework/common/guide/server/01-overview.ko.md) |
| Framework 정식 계약 | [Framework 공통 스펙](./framework/doc/framework/common/spec/README.ko.md) |
| Framework 내부 구조 | [Framework internals](framework/doc/framework/common/spec/README.ko.md) |
| Core를 소스에서 빌드하고 테스트 | [Core 빌드 가이드](./doc/building/build-guide.ko.md) |
| 현재 source로 Core와 Bindings local package 생성 | [Local package 가이드](./scripts/local-package/README.ko.md) |
| Binding package 설치와 사용 | [Bindings 가이드](./bindings/doc/guide/README.ko.md) |
| Framework package 설치와 첫 실행 | [Framework 시작하기](./framework/doc/framework/common/guide/server/02-getting-started.ko.md) |
| Framework runtime source/build 진입점 | [C++](./framework/languages/cpp/) · [.NET](./framework/languages/dotnet/) · [JVM](./framework/languages/java/) · [Node.js](./framework/languages/node/) |
| 릴리스 package 구성 | [패키징 가이드](./doc/building/packaging.ko.md) |
| 라이선스 정책 | [라이선스 안내](./doc/license/README.ko.md) |
| 보안 취약점 보고 | [보안 정책](./SECURITY.md) |

가이드는 개념과 사용법을 설명하고, 스펙과 언어별 exact interface가 정식 계약을
소유합니다. 두 내용이 다르면 스펙과 exact interface를 우선합니다.

## 지원 플랫폼

Core는 Linux, macOS와 Windows의 x64/ARM64 빌드 경로를 제공합니다. Binding과
Framework의 지원 runtime, package 형식과 플랫폼 제약은 각 언어 문서에서
확인하세요.

## 라이선스

이 저장소는 계층에 따라 라이선스가 다릅니다.

| 범위 | 라이선스 |
|---|---|
| `core/`, `bindings/` | [Mozilla Public License 2.0](./LICENSE) |
| `framework/` | [Functional Source License 1.1, ALv2 Future License](./framework/LICENSE) |
| Framework 언어별 `http-client` package | Apache License 2.0 |

세부 사용 조건, 2년 후 Apache License 2.0 전환 정책과 재배포 고지는
[라이선스 안내](./doc/license/README.ko.md)와
[THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md)를 참고하세요.

[libzmq](https://github.com/zeromq/libzmq) 기반 — Copyright (c) 2007-2024
Contributors as noted in [`core/AUTHORS`](./core/AUTHORS).

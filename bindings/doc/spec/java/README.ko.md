---
title: "Java 바인딩 구현 청사진"
---

<!-- bindings-nav:start -->
[스펙 목록](../README.ko.md) | [이전: C++](../cpp/README.ko.md) | [다음: Node.js](../node/README.ko.md)
<!-- bindings-nav:end -->

# Java 바인딩 구현 청사진

> **이 장이 정의하는 것** — Java 바인딩이 갖춰야 할 `contracts`/`runtime`
> 목표 형태와 JPMS export 경계.

이 문서는 Java 바인딩의 목표 형태를 정의한다. 모든 메서드를 빠짐없이 나열하는
레퍼런스는 아니다. 정확한 public 멤버 목록은 리팩터가 완료된 뒤
`bindings/java/src/main/java/systems/zlink/contracts/` 아래에 둔다.

Java 바인딩은 .NET 바인딩과 동일한 architecture map을 사용할 때만 정렬된 것으로
본다:

- public resource 동작은 contract interface로 표현한다;
- native-backed runtime 구현은 runtime 패키지 아래에 둔다;
- 생성은 public factory entry point를 통해 흐른다;
- DTO, value, record, enum, result, exception 타입은 concrete로 유지한다;
- runtime/native 세부는 public contract signature에 등장하지 않는다;
- 테스트, 샘플, perf, 애플리케이션은 public contract 패키지만 import한다.

Java public contract 분류는
[.NET 바인딩 청사진](../dotnet/README.ko.md)을 기준선으로 따른다. Java의 public 타입
규칙 때문에 어색해지는 경우 모든 C# 파일을 문자 그대로 복사할 필요는 없다.
다만 동일한 카테고리 소유권, resource 경계, operation/model 그룹은 보존해야
한다.

이 문서는 breaking target이다. 옛 Java surface를 보존하려는 호환 shim,
deprecated wrapper, 중복 생성 경로, public runtime alias, direct constructor를
유지하지 않는다.

| 절 | 다루는 내용 |
|---|---|
| [Source Of Truth](#source-of-truth) | 의미 원천과 Java 저장소 소유권 경계 |
| [Current Refactor Rule](#current-refactor-rule) | 리팩터 진행 중 "옳은 방향"을 판정하는 기준 |
| [Architecture Map](#architecture-map) | `contracts`/`internal`/`runtime` 패키지 트리 |
| [Public Contract Categories](#public-contract-categories) | contract·runtime 패키지 → 목적 표 |
| [Native Wait Boundary](#native-wait-boundary) | blocking recv와 poller 기반 수신의 경계 |
| [Proposed Repository Layout](#proposed-repository-layout) | Gradle 프로젝트 전체 디렉터리 트리 |
| [Contract Interface Rule](#contract-interface-rule) | interface로 남을 타입과 concrete로 남을 타입 |
| [Factory Entry Points](#factory-entry-points) | root/context/service factory 메서드 |
| [Contract File Requirements](#contract-file-requirements) | contract 파일이 import할 수 있는 것/없는 것 |
| [Runtime Implementation Requirements](#runtime-implementation-requirements) | runtime이 소유하는 구현 세부 |
| [Socket Contract Shape](#socket-contract-shape) | 공통/타입별 socket 동작 |
| [Operation Builder Shape](#operation-builder-shape) | builder 시작 메서드와 terminal 메서드 |
| [Messaging Values](#messaging-values) | `Message`/`Received`/`TopicMessage`/`SubscriptionEvent` 계약 |
| [Receive And Subscribe Shape](#receive-and-subscribe-shape) | 호출자 제공 저장소, no-data, Core HWM ownership 경계 |
| [Handler Registration Naming](#handler-registration-naming) | `set...Handler` 명명 규칙 |
| [Byte HWM 및 monitoring ABI v4](#byte-hwm-및-monitoring-abi-v4) | 양수 `long` HWM과 monitor snapshot field |
| [Receive flow state](#receive-flow-state) | receive-flow 상태 타입, setter와 monitor 표면 |
| [Error And Result Policy](#error-and-result-policy) | typed exception과 검증 시점 |
| [Spot And Actor Contract Shape](#spot-and-actor-contract-shape) | `SpotNode`/`Spot` 책임과 route 결과 |
| [Spot Get-Or-Create](#spot-get-or-create) | `getOrCreateSpot` 계약 |
| [Performance Policy](#performance-policy) | hot path 제약 |
| [Refactor Workflow](#refactor-workflow) | 정렬 작업 순서 |
| [Implementation Checklist](#implementation-checklist) | 정렬 선언 전 확인 항목 |
| [Verification](#verification) | 필수 검증 명령과 구조 검색 |

## Source Of Truth

의미적 진실의 원천은 `core/include/zlink.h`이다. 공유되는 바인딩 정책은
`doc/spec/bindings/README.md`이다. .NET 투영은
[.NET 바인딩 청사진](../dotnet/README.ko.md)이며, Java는 그 설계를 따르되 Java 패키지
이름과 Java 네이밍 컨벤션을 사용한다.

Java 저장소의 소유권 경계는 다음과 같다:

- Public contract 소스:
  `bindings/java/src/main/java/systems/zlink/contracts/`.
- Native-backed runtime 구현:
  `bindings/java/src/main/java/systems/zlink/runtime/`.
- Native bridge와 Panama/JNI downcall:
  `bindings/java/src/main/java/systems/zlink/runtime/nativeapi/`.
- Native artifact와 resource:
  `bindings/java/src/main/resources/native/`와 `bindings/java/native/`.
- 테스트: `bindings/java/src/test/`와 `bindings/java/tests/`.
- 샘플: `bindings/java/samples/`.
- Perf: `bindings/java/perf/`.

JPMS export는 `systems.zlink.contracts.*` 아래의 문서화된 패키지만 포함한다.
`systems.zlink.runtime.*` 아래 패키지는 `systems.zlink.runtime.nativeapi`를
포함해 구현 패키지이며 export하지 않는다.

## Current Refactor Rule

Java 리팩터 중에는 코드가 잠시 전이 상태일 수 있으나 목표 형태는 고정되어 있다.
변경이 다음 진술 중 하나를 더 참이 되게 만들 때에만 옳은 방향으로 움직인 것으로
본다:

- native-backed resource가 public contract interface로 타입 지정된다;
- native-backed 구현이 `systems.zlink.runtime.*` 아래로 이동한다;
- factory가 contract 타입을 반환하고 runtime 클래스를 감춘다;
- public contract 파일이 더 이상 `systems.zlink.runtime.*`을 import하지 않는다;
- 샘플, perf runner, 테스트가 더 이상 runtime 패키지를 import하지 않는다;
- native handle, raw part loop, callback trampoline, Core reply/send completion,
  native
  struct mirror가 public contract 소스 밖으로 이동한다.

native helper만 옮기고 주요 public resource는 concrete contract 클래스로 두는
것은 충분하지 않다. .NET 표준 목표는 resource 경계에서의 contract/runtime
분리이며, helper 경계에서의 분리만으로는 부족하다.

## Architecture Map

Java는 .NET과 동일한 개념적 map을 소문자 패키지 이름으로 표현한다. 패키지 이름은
Java에 맞추되 소유권 규칙은 동일하다.

```text
bindings/java/src/main/java/systems/zlink/
+-- contracts/
|   +-- core/
|   +-- messaging/
|   +-- sockets/
|   +-- eventing/
|   +-- service/
|   |   +-- spot/
|   +-- errors/
+-- internal/
+-- runtime/
|   +-- core/
|   +-- messaging/
|   +-- sockets/
|   +-- eventing/
|   +-- service/
|   |   +-- spot/
|   +-- errors/
|   +-- nativeapi/
```

`contracts`는 public API map이다. 리뷰어는 이 트리와 public factory만 읽고도
사용자가 관찰 가능한 모든 동작을 이해할 수 있어야 한다.

`internal`은 non-exported bridge map이다. Contract 소유 상태와 runtime 구현을
연결해야 하지만 그 hook을 애플리케이션용 API로 만들면 안 되는 코드만 둔다.

`runtime`은 구현 map이다. Java 목표 분류인 `core`, `messaging`, `sockets`,
`eventing`, `service`, `errors`, 그리고 `.NET`의 `Runtime/Native`에 대응하는
Java의 `nativeapi`를 사용한다. native handle, downcall, marshalling, callback
bridge 상태, Core reply/send completion, socket kernel, service kernel, option mapping,
lifecycle 세부를 소유한다. handle lifetime, buffer conversion, option mapping
같은 runtime support 코드는 별도 public package category를 늘리지 않고 해당
runtime 소유 카테고리 안에 둔다.

두 트리가 일대일 파일 대응을 가질 필요는 없다. 단, 소유권은 분명해야 한다.
모든 native-backed resource에는 public contract 소유자와 runtime 구현 소유자가
존재해야 한다.

## Public Contract Categories

Java contract 카테고리는 규범적이다.

| 패키지 | 목적 |
| ------ | ---- |
| `systems.zlink.contracts.core` | 라이브러리 진입점, context resource contract, routing id, version/capability 조회 helper, process 레벨 helper. |
| `systems.zlink.contracts.messaging` | 메시지 값, 수신 envelope, topic message, subscription event, payload ownership, 공통 메시지 메타데이터. |
| `systems.zlink.contracts.sockets` | Socket resource contract, socket operation builder, socket option, send/recv/request/reply/publish surface. |
| `systems.zlink.contracts.eventing` | Poller, poll event, monitor socket, monitor snapshot, timer resource contract. |
| `systems.zlink.contracts.service.spot` | SpotNode, Spot, Actor, route/admission handler, actor lifecycle, service operation builder. |
| `systems.zlink.contracts.errors` | Public exception과 typed error/result 도메인. |

Runtime 패키지는 동일한 .NET 표준 분류를 Java 패키지 이름으로 사용한다:

| 패키지 | 목적 |
| ------ | ---- |
| `systems.zlink.runtime.core` | Context 구현, context option 적용, runtime version/capability 조회 호출. |
| `systems.zlink.runtime.messaging` | 메시지 materialization, multipart progress, request 실행과 Core callback 연결. |
| `systems.zlink.runtime.sockets` | Socket kernel, socket family 구현, callback adapter, socket operation 실행. |
| `systems.zlink.runtime.eventing` | Monitor, poller, poll event, timer, dispatch loop 구현. |
| `systems.zlink.runtime.service.*` | SpotNode, Spot, Actor, topology, service operation 구현. |
| `systems.zlink.runtime.errors` | Native errno/result를 public exception/result 도메인으로 변환. |
| `systems.zlink.runtime.nativeapi` | JNI/Panama 선언, ABI mirror, 심볼 로딩, native artifact lookup. |

Enum, flag, result 타입은 그 의미를 부여하는 개념과 함께 둔다. `enums`나
`callbacks` 같은 문법 전용 public Java package는 만들지 않는다.
`SocketEnums/` 같은 물리 source 폴더는 Java `package` 선언이 소유 contract
패키지로 유지될 때에만 파일 분류 그룹으로 허용한다.

## Native Wait Boundary

Java 바인딩은 low-level socket recv API와 poller 기반 수신 경계를 구분한다.

- `socket.recv(received, RecvFlags.NONE)`은 native blocking recv다. 적은 수의 전용
  thread나 단순 테스트에서 직접 사용할 수 있다.
- framework나 많은 client session을 처리하는 runtime은 blocking recv를 handler 실행 thread에
  직접 올리지 않는다. runtime은 `Poller`로 readiness를 기다린 뒤 ready socket에 대해
  `RecvFlags.DONT_WAIT` recv를 수행한다.
- application handler는 native wait thread가 아니라 framework가 설정한 handler executor 뒤에서
  실행된다. virtual thread를 사용한다면 이 handler executor 경계에서 사용한다.
- 별도 public dispatcher API는 제공하지 않는다. 기존 `Poller`, socket `recv(..., DONT_WAIT)`,
  framework 내부 수신 루프로 충분하며, bindings public 계약에 framework 실행 정책을 섞지 않는다.

## Proposed Repository Layout

Java 바인딩 저장소 레이아웃의 리뷰 대상은 다음과 같다. `.NET`과 동일한 public
contract 분류를 유지하면서 Java 패키지 이름과 Java 파일 규칙을 사용한다. 샘플과
perf 디렉터리는 기존 Gradle 프로젝트 형태를 유지하며, 분류 규칙은 그 안의 source
package tree에 적용한다.

```text
bindings/java/
+-- build.gradle.kts
+-- settings.gradle.kts
+-- gradle/
+-- codec/
|   +-- zlink-ext-netty/
+-- native/
|   +-- linux-x64/
|   +-- linux-x86_64/
|   +-- src/
+-- src/
|   +-- main/
|   |   +-- java/
|   |   |   +-- module-info.java
|   |   |   +-- systems/zlink/contracts/
|   |   |   +-- systems/zlink/runtime/
|   |   +-- resources/native/
|   |       +-- darwin-aarch64/
|   |       +-- darwin-x86_64/
|   |       +-- linux-aarch64/
|   |       +-- linux-x64/
|   |       +-- linux-x86_64/
|   |       +-- windows-aarch64/
|   |       +-- windows-x86_64/
|   +-- test/
|       +-- java/systems/zlink/
|           +-- contract/
|           +-- integration/
+-- tests/
|   +-- run_tests.sh
|   +-- certs/
+-- samples/
|   +-- Zlink.Samples/
|       +-- src/main/java/systems/zlink/samples/
+-- perf/
    +-- common/
    +-- multi/Zlink.BindingBench.Multi/
    +-- single/Zlink.BindingBench/
    +-- baseline/
    +-- results/
    +-- tests/
```

`contracts`만 public Java API 트리이다. `runtime`, `native`,
`src/main/resources/native`는 구현 및 패키징 트리이다. 샘플, perf,
애플리케이션용 테스트는 `systems.zlink.contracts.*`만 import한다.
`systems.zlink.runtime.*`은 import하지 않는다.

`systems/zlink/internal/`은 contract 소유 public helper와 runtime 구현 사이를
잇는 non-exported bridge로만 존재할 수 있다. Public contract 패키지가 아니며
샘플, perf, 애플리케이션은 import하지 않는다.

### Public Contract Layout

이 트리는 [.NET 바인딩 청사진](../dotnet/README.ko.md)이 정의한 contract 카테고리의
Java 투영이다. 아래 그룹 디렉터리는 Java 트리를 .NET과 유사한 수준으로 읽기
쉽게 만들기 위한 물리 source-file 그룹이다. 추가 public Java package 이름을
만들지는 않는다. 예를 들어 `contracts/sockets/SocketEnums/SendResult.java`는
여전히 `package systems.zlink.contracts.sockets;`를 선언한다. 이렇게 해야 Java
패키지 관례와 public import 경로를 유지하면서도 요구한 파일 분류를 제공할 수
있다.

```text
systems/zlink/contracts/
+-- core/
|   +-- AtomicCounter.java
|   +-- Context.java
|   +-- ContextOption.java
|   +-- ContextOptions.java
|   +-- RoutingId.java
|   +-- Stopwatch.java
|   +-- Zlink.java
|   +-- ZlinkThread.java
|   +-- ZlinkVersion.java
+-- errors/
|   +-- Errors/
|       +-- *Exception.java
|       +-- *Result.java
|       +-- ErrorCode.java
+-- eventing/
|   +-- EventEnums/
|   +-- EventHandlers/
|   +-- EventModels/
|   +-- MonitorSocket.java
|   +-- Poller.java
|   +-- Timer.java
+-- messaging/
|   +-- Message.java
|   +-- Received.java
|   +-- SubscriptionEntry.java
|   +-- SubscriptionEvent.java
|   +-- TopicMessage.java
+-- service/
|   +-- spot/
|   |   +-- SpotRoute.java
|       +-- Actor.java
|       +-- Spot.java
|       +-- SpotDispatchInfo.java
|       +-- SpotNode.java
|       +-- ActorJoinOperations/
|       +-- ActorManagementOperations/
|       +-- ActorModels/
|       +-- ServiceEnums/
|       +-- SpotNodeModels/
|       +-- SpotOperations/
|       +-- TopologyEnums/
+-- sockets/
    +-- Socket.java
    +-- StreamSocket.java
    +-- MessageSocketContracts/
    +-- PubSubSocketContracts/
    +-- RoutedSocketContracts/
    +-- SocketEnums/
    +-- SocketHandlers/
    +-- SocketOperations/
    +-- SocketOptionFacades/
```

`systems/zlink/internal/ContractAccess.java` 같은 internal bridge 파일은
export되지 않고 애플리케이션용 API가 아니므로 의도적으로 `contracts/` 트리 밖에
둔다.

그룹 디렉터리는 임의의 기능 묶음이 아니다.
[.NET 바인딩 청사진](../dotnet/README.ko.md)이 정의한 contract 그룹의 Java source
file 그룹이다. 새 public contract 파일은 그 개념을 소유하는 가장 작은 그룹에
둔다.

Java public 이름은 Java 이름이어야 한다. C#의 `I` 접두사는 복사하지 않는다:
`.NET`의 `ISocket.cs`는 Java의 `Socket.java`에, `IStreamSocket.cs`는 Java의
`StreamSocket.java`에 대응한다.

### Runtime Layout

Runtime 트리는 public contract 트리를 비추되, 구현 소유자를 찾는 데 도움이 될
때에만 그렇게 한다. Public API가 아니며 JPMS로 export하지 않는다. Runtime 파일은
각 runtime 카테고리에 맞는 일반 Java package 선언을 사용한다.

```text
systems/zlink/runtime/
+-- core/
|   +-- NativeAtomicCounter.java
|   +-- NativeContext.java
|   +-- NativeCoreResources.java
|   +-- NativeCoreRuntime.java
|   +-- NativeRuntimeFactory.java
|   +-- NativeStopwatch.java
|   +-- NativeZlinkThread.java
+-- messaging/
|   +-- NativeMessageRuntime.java
|   +-- ReceivedPartCursor.java
+-- sockets/
|   +-- NativeSocketBase.java
|   +-- NativeSocketRuntime.java
|   +-- NativeSockets.java
|   +-- NativePairSocket.java
|   +-- NativeDealerSocket.java
|   +-- NativeRouterSocket.java
|   +-- NativePubSocket.java
|   +-- NativeSubSocket.java
|   +-- NativeXPubSocket.java
|   +-- NativeXSubSocket.java
|   +-- NativeStreamSocket.java
|   +-- NativeRouterReceiveSupport.java
|   +-- NativeRouterRequestSupport.java
|   +-- NativeRouterSpotSupport.java
|   +-- NativeStreamActorSupport.java
|   +-- SocketOperations.java
+-- eventing/
|   +-- NativeMonitorSocket.java
|   +-- NativePollEvents.java
|   +-- NativePoller.java
|   +-- NativeTimer.java
+-- service/
|   +-- spot/
|       +-- NativeActor.java
|       +-- NativeSpot.java
|       +-- NativeSpotNode.java
|       +-- SpotOptions.java
|       +-- SpotRoutedSupport.java
+-- errors/
|   +-- NativeErrorRuntime.java
+-- nativeapi/
    +-- Native.java
    +-- NativeLayouts.java
    +-- NativeMsg.java
    +-- NativeHelpers.java
    +-- NativeSymbols.java
    +-- LibraryLoader.java
    +-- InternalAccess.java
```

Runtime support 파일은 목표 runtime 카테고리 안에서 실제 구현 복잡성을 감출
때에만 허용한다. `NativeRouterSocket`, `NativeSpotNode`, `NativePoller` 같은
resource 소유자의 대체물이 아니다.

## Contract Interface Rule

오직 native-backed resource 동작과 staged operation 동작만 interface가 된다.
값 성격의 타입은 concrete로 유지한다.

### Must Be Public Interfaces

다음은 resource contract이다. Runtime 구현은 이 interface를 구현해야 하고
factory가 생성해야 한다.

- `Context`
- `Socket`
- `PairSocket`
- `DealerSocket`
- `RouterSocket`
- `PubSocket`
- `SubSocket`
- `XPubSocket`
- `XSubSocket`
- `StreamSocket`
- `MonitorSocket`
- `Poller`
- `Timer`
- `SpotNode`
- `Spot`
- Java surface가 actor handle 또는 actor lifecycle resource를 native-backed
  handle로 노출할 때의 Actor resource contract

다음은 staged multipart 상태, request 상태, callback 상태, native submit
상태를 감추기 때문에 operation contract이다:

- send operation
- routed send operation
- publish operation
- request operation
- reply operation
- SPOT send/request/reply operation
- Actor create/join/reply/location operation
- stream actor bind/unbind/send operation

호출자가 runtime에 동작을 제공하는 경우 handler와 callback 역할은 interface
또는 functional interface가 될 수 있다:

- socket receive handler
- stream packet handler
- monitor handler
- timer handler
- SPOT dispatch handler
- actor lifecycle handler
- request callback
- reply callback

### Must Stay Concrete

대칭성만을 위해 다음을 interface로 만들지 않는다:

- `Message`
- `Received`
- `TopicMessage`
- `SubscriptionEvent`
- `RoutingId`
- option과 filter 값 객체
- route result 모델
- snapshot 모델
- actor 참조
- enum/flag/result 타입
- exception

이들은 값 또는 result 객체다. 내부적으로 native-backed 저장소를 소유할 수
있으나, 호출자는 이들에 대해 교체 가능한 동작을 필요로 하지 않는다.

## Factory Entry Points

생성은 contract factory를 통해서만 public이다. Runtime 구현의 직접 생성은
목표 API의 일부가 아니다.

### Root Factory

`Zlink`는 `systems.zlink.contracts.core`에 둔다.

필수 root factory 메서드:

- `Zlink.createContext()`
- `Zlink.createPoller()`
- `Zlink.createTimer()`
- `Zlink.createTimer(Spot spot)`

`Zlink`는 version, capability 조회, strerror, proxy, shutdown, sleep, auto-HWM
재계산 같은 public static helper도 소유할 수 있다. 이 helper들은 runtime/native
코드에 위임해도 되지만, public signature는 runtime 패키지나 native bridge 타입을
언급하지 않는다.

### Context Factories

`Context`는 `systems.zlink.contracts.core`의 public interface다.

필수 context factory 메서드:

- `createPairSocket()`
- `createDealerSocket()`
- `createRouterSocket()`
- `createPubSocket()`
- `createSubSocket()`
- `createXPubSocket()`
- `createXSubSocket()`
- `createStreamSocket()`
- `createSpotNode(...)`

모든 factory는 public contract interface 또는 concrete 값 타입을 반환한다.
`NativeContext`, `NativeRouterSocket`, `NativeSpotNode` 같은 runtime 클래스를
반환하지 않는다.

### Service Factories

SPOT과 Actor handle은 `SpotNode` 또는 다른 contract 소유 service 객체의 service
메서드만으로 생성한다.

허용되는 SPOT 생성 패턴:

- `SpotNode.createSpot(...)`
- `SpotNode.entrySpot()`
- `SpotNode.getOrCreateSpot(...)`
- `SpotNode.spotLookup(...)`

허용되는 Actor 생성 패턴:

- `SpotNode.createActor(...)`
- `SpotNode` 또는 `Spot`이 명시적으로 소유한 actor factory/service 메서드

`Spot`, `SpotNode`, `Actor` 또는 runtime service 클래스의 직접 public
constructor는 목표 contract의 일부가 아니다.

## Contract File Requirements

Contract 파일은 Panama, JNI, native handle, native struct layout, callback
userdata, Core callback thread, raw `*_part` loop를 몰라도 읽을 수 있어야 한다.

Contract 파일이 import할 수 있는 것:

- 다른 `systems.zlink.contracts.*` 패키지;
- public signature에 필요한 JDK 타입, 예를 들어 `Duration`, `AutoCloseable`,
  `CompletionStage`, `Optional`, `List`, record, functional interface;
- public contract의 의도된 일부인 경우에 한해 서드파티 public 값 타입.

Contract 파일이 import할 수 없는 것:

- `systems.zlink.runtime.*`;
- `systems.zlink.runtime.nativeapi.*`;
- public native/Panama 세부를 위한 `java.lang.foreign.*`;
- runtime 구현 클래스;
- native handle wrapper;
- marshalling helper;
- Core reply callback helper.

유일한 예외는 Java가 직접 static 생성 와이어링을 선택한 경우의 `Zlink` 같은
public factory facade이다. 그 경우에도 runtime 참조는 메서드 본문 안의 private
구현 세부여야 하며 public signature에 등장해선 안 된다. Contract 파일을 깨끗하게
유지하는 데 도움이 된다면 작은 runtime factory bridge를 두는 편을 선호한다.

## Runtime Implementation Requirements

Runtime 클래스는 public contract interface를 구현한다. Contract interface,
concrete 값 타입 또는 문서화된 factory에서 찾을 수 없는 추가적인 사용자 관찰
가능 동작을 도입해선 안 된다.

Runtime 클래스는 Java 패키지나 JPMS 메커니즘 때문에 `public`일 수 있지만,
`systems.zlink.runtime.*`이 export되지 않으므로 public API가 아니다. 샘플, perf,
애플리케이션, contract 테스트는 이를 import하지 않는다.

Runtime이 소유하는 것:

- native handle lifecycle;
- close와 idempotent cleanup 규칙;
- native downcall;
- native struct mirror;
- 메시지 marshalling;
- Core request reply callback registry와 send-completion callback registry;
- callback trampoline;
- receive cursor;
- part-loop sequencing;
- native error mapping;
- typed option mapping;
- native resource 채택과 해제;
- native data로부터 디코딩된 service snapshot.

Runtime 구현이 concrete 값 객체에 대한 package-private 접근이 필요하면
runtime/nativeapi가 소유하는 좁은 내부 bridge를 사용한다. native handle이나
내부 필드를 public contract에 노출하지 않는다.

## Socket Contract Shape

Socket contract는 interface다. native transport 메커니즘이 아니라 동작을
드러내야 한다.

공통 socket 동작은 `Socket`에 둔다:

- `bind`
- `connect`
- `unbind`
- `disconnect`
- `disconnectRid`
- `setChannelName`
- `getChannelName`
- `options`
- `close`

Typed socket contract는 해당 socket 타입에 의미 있는 기능만 더한다:

- `PairSocket`: send와 recv.
- `DealerSocket`: send, recv, request.
- `RouterSocket`: routed send, routed recv, request, reply, SPOT routing와
  monitor event의 exact transport pair 종료.
- `PubSocket`: publish.
- `SubSocket`: subscribe와 subscription event 수신.
- `XPubSocket`: publish와 subscription event 수신.
- `XSubSocket`: public 바인딩 contract가 정의하는 send와 subscription 제어.
- `StreamSocket`: stream send/recv, packet handler, actor gateway, bound actor
  operation.

protocol envelope helper, request token, raw native part submission, callback
userdata, native routing-id pointer는 노출하지 않는다.

## Operation Builder Shape

Operation builder는 변경 가능한 staged 상태를 감추기 때문에 public interface다.
operation을 소유하는 카테고리 안에 둔다.

Builder 시작 메서드는 대상 식별자만 받는다:

- `send()`
- `send(routingId)`
- `publish(topic)`
- `request()`
- `request(routingId)`
- `reply(routingId, requestSequence)`
- `sendToSpot(nodeRid, spotRid)`
- `requestToSpot(nodeRid, spotRid)`
- `replyToSpot(nodeRid, spotRid, requestSequence)`
- `sendBoundActor(sessionRid, actorId)`

`RouterSocket.disconnectTransportPair(transportPairId, transportPairGeneration)`은
같은 monitor event에서 얻은 non-zero pair identity와 generation에 해당하는
physical transport pair만 종료 대상으로 지정한다. 같은 peer routing id를
사용하는 다른 pair에는 영향을 주지 않는다. 이 메서드는 transport pair
identity가 필요한 Framework connection replacement와 같은 runtime 제어에
사용하며, 임의의 pair identity를 새로 만들어 전달하지 않는다.

PAIR send와 DEALER/ROUTER routed send, request builder의 canonical terminal은
인자 없는 `submit()` 하나다. PAIR send와 routed send는
`CompletionStage<Void>`, request는 `CompletionStage<List<Message>>`를 반환한다.
PUB/XPUB publish도 같은 staged message builder를 사용하지만 `submit()`은
동기 `void`이며, 성공하지 못하면 즉시 `ZlinkSubmitException`을 던진다.

```java
public interface AsyncSendSubmitOperation {
    AsyncSendSubmitOperation message(Message part);
    AsyncSendSubmitOperation timeout(Duration timeout);
    CompletionStage<Void> submit();
}

public interface RoutedSendSubmitOperation {
    RoutedSendSubmitOperation message(Message part);
    RoutedSendSubmitOperation timeout(Duration timeout);
    CompletionStage<Void> submit();
}

public interface RequestSubmitOperation {
    RequestSubmitOperation message(Message part);
    RequestSubmitOperation timeout(Duration timeout);
    CompletionStage<List<Message>> submit();
}
```

이 비동기 builder에는 blocking `await()`, `submit(callback)`, `flags(...)`, boolean
one-shot terminal을 제공하지 않는다. `submit()`은 호출 thread를 막지 않으며
Framework와 Kotlin은 반환된 `CompletionStage`를 직접 completion/await 경계에
연결한다. Kotlin의 canonical 사용은 `submit().await()`다. 언어별 비동기 실행 표면 기준은
[바인딩 비동기 실행 표면 정책](../async-coroutine-policy.ko.md)을 따른다.

PUB/XPUB publish builder의 `submit()`은 `CompletionStage`를 만들지 않는다. 기본
lossy publish는 subscriber queue가 가득 차도 해당 subscriber 복사본을 버리고
성공을 반환하며, `NODROP`은 즉시 오류를 반환한다. PUB/XPUB에
`zlink_send_async`를 호출하면 Core가 `ENOTSUP`을 반환한다.

Raw ROUTER/`Received` reply의 terminal은
`ReplySubmitOperation.submit() -> void`인 동기 one-shot이다. `CompletionStage`를
반환하지 않고 terminal reply 또는 error reply를 HWM 없는 completion lane에 native 호출
한 번으로 제출한다. HWM backpressure는 reply 결과가 아니며 `NOT_CONNECTED`,
`TERMINATED`, `INVALID_ARGUMENT`와 그 밖의 non-HWM submit 실패는 즉시
`ZlinkSubmitException`으로 전달한다.

### Core completion으로 비동기 operation 완료

- 각 PAIR/DEALER/ROUTER/STREAM socket에는 `zlink_send_complete_handler`를
  하나만 설치한다. routed target은 Core selector 또는 명시된 exact transport-pair
  identity로 선택하며, binding이 target별 admission queue나 readiness ring을
  소유하지 않는다.
- `submit()`은 Core 호출 전에 CompletionStage와 binding-owned opaque userdata
  token을 strong pending table에 등록한다. Core가 `zlink_send_async` 안에서
  callback을 inline 호출해도 안전하며, callback은 table에서 항목을 한 번만 꺼내
  stage를 완료한다.
- Completion callback은 Core가 호출한 JVM thread에서 completion만 전달한다.
  필요한 JVM attach/detach는 허용되지만 binding은 admission용 thread, queue,
  scheduler, retry를 만들지 않는다. deadline은 per-operation Core option이다.
- `TIMED_OUT`와 `TERMINAL`은 `terminal_errno`를 보존한
  `ZlinkSubmitException`으로 stage를 예외 완료한다. 취소는
  `zlink_send_async_cancel`을 요청하며 Core completion은 여전히 정확히 한 번
  전달된다. callback 안에서 다시 submit하는 것은 Core 계약상 `EDEADLK`다.
- Request는 request callback table을 마지막 request part보다 먼저 설치하고,
  Core reply callback이 반환 stage를 직접 완료한다. timeout scheduler나
  completion executor를 binding이 소유하지 않는다.
- 현재 Core의 ROUTER multipart abort와 DEALER generic-target-fail 결함 때문에
  Java multipart-async contract 검증은 한 part record로 제한한다. Core 수정 후
  multipart assertion을 복원한다.

`sendNoWait`, `sendWithFlags`, `requestAsync`, `publishWithFlags`,
`send(message)` shortcut 같은 별도의 operation-start 계열을 추가하지 않는다.
하나의 operation 이름을 사용하고 변형은 builder가 흡수한다.

## Messaging Values

`Message`, `Received`, `TopicMessage`, `SubscriptionEvent`는 concrete contract
타입이다.

`Message`:

- 문서화된 ownership 규칙에 따라 메시지 payload를 소유하거나 공유한다;
- `Message.from(...)` 같은 Java 친화적 factory를 노출한다;
- raw `wrapNative`, `wrapDirect`, native pointer, borrow된 Java buffer send
  경로를 public API로 노출하지 않는다;
- receive wrapper는 owner `Received`가 part reference를 제거하고 `close()`를 완료한 뒤
  bounded `ThreadLocal` pool에 반환될 수 있다;
- deterministic cleanup이 반환되면 그 `Message` reference는 무효다. 반복 `close()`, payload
  접근, identity 기반 `Map`/`WeakMap` 조회를 포함해 다시 사용하면 안 된다. 반환되지 않은
  wrapper와 사용자가 직접 생성한 owned `Message`는 다른 ownership에 재사용하지 않는다.

`Received`:

- 호출자가 제공하는 재사용 가능한 수신 저장소다;
- close 또는 채택될 때까지 수신된 메시지 part를 소유한다;
- routing id, SPOT routing id, request sequence, reply sender 메타데이터를
  가질 수 있다;
- native receive cursor나 native handle을 노출하지 않는다.

`TopicMessage`와 `SubscriptionEvent`:

- concrete result/storage 타입이다;
- public receive/subscribe API를 통해 채워진다;
- raw native topic buffer를 노출하지 않는다.

## Receive And Subscribe Shape

데이터 평면 receive API는 호출자가 제공한 저장소를 사용하고 `boolean`을
반환한다.

목표 형태 예시:

```java
Received received = new Received();
boolean ok = router.recv(received, RecvFlags.DONT_WAIT);
```

호출자가 제공하는 no-wait receive에서 no-data는 정상적인 `false` 결과다. 하드
수신 실패는 문서화된 exception 타입을 던진다.

Core byte HWM charge는 일반 `recv(...)`와 `subscribe(...)`가 payload를 dequeue할
때 끝난다. `Received`와 `TopicMessage`는 part, Routing ID, request sequence,
topic과 multipart framing의 Java 수명만 소유한다. `close()`와 다음 수신 저장소
재사용은 payload와 metadata를 정리하지만 Core HWM accounting에는 관여하지 않는다.
별도 retained receive, raw lease handle, application byte capacity 또는 중복
accounting 상태는 public이나 internal API에 두지 않는다.

SPOT readable dispatch 이벤트는 readiness 알림이다. 호출자는 대응하는 receive
API를 no-data가 될 때까지 drain한다.

service 제어/admission receive API는 재사용 가능한 데이터 평면 저장소보다 더
명확할 때 `Optional`, nullable, typed result-return 형태를 사용할 수 있다. 이
경우에도 no-data와 하드 수신 실패를 구분해야 한다.

`ReceiveRecord.sourceBindingGeneration()`은 bound STREAM session에서 Actor로
전달한 record의 검증된 binding generation을 반환한다. 이 경우
`sourceSpotRid()`은 session routing ID를 반환한다. 다른 record에서는 Core가
전달한 0을 유지한다.

Mesh dispatch의 `SEND_READY` record는 `MeshSendReadyData`로 decode한다. 이 값은
Core의 destination kind와 target node RID, target Spot RID, target Actor ref,
channel name을 그대로 보존한다. 해당 destination kind에 사용하지 않는 필드는
Core가 전달한 empty value로 유지한다. `ReceiveRecord.sendReady()`는 kind data가
이 타입일 때만 값을 반환하며 다른 record kind에는 `null`을 반환한다.
이 record kind는 service-wire dispatch protocol이며 Core HWM send-ready callback이나
async send completion을 뜻하지 않는다.

## Handler Registration Naming

Handler 등록 이름은 이벤트 발생이 아니라 등록을 설명한다.

- 한 주제당 하나의 활성 handler에는 `set...Handler`를 사용한다.
- 같은 setter를 다시 호출하면 handler가 교체된다.
- public contract가 의도적으로 다중 활성 handler를 지원할 때에만 `add...Handler`
  또는 `register...Handler`를 사용한다.
- public 등록의 표준 이름으로 `on...`을 사용하지 않는다.

표준 Java 이름:

- `setPacketHandler`
- `setDispatchHandler`
- `recvRouted`
- `recvActorLifecycle`

## Byte HWM 및 monitoring ABI v4

- HWM은 queue의 message 수가 아니라 Core가 계산한 accounted byte의 상한이다.
- Java 공개 interface는 `0`부터 `Long.MAX_VALUE`까지의 byte 값을 허용한다. 음수 입력은
  Core를 호출하기 전에 거부하고, `Long.MAX_VALUE`보다 큰 Core 조회값은 overflow 오류로 처리한다.
- `0`은 무제한이고 수동 기본값은 `4_096_000 bytes`다.
- 이전 `int` overload, alias 또는 count 단위 adapter는 제공하지 않는다.

```java
public final class ContextOptions {
    public long coreHwmMemoryLimitBytes();
    public void coreHwmMemoryLimitBytes(long value);
    public long coreHwmBudgetBytes();
    public void coreHwmBudgetBytes(long value);
    public CoreHwmProfile coreHwmProfile();
    public void coreHwmProfile(CoreHwmProfile value);
}

public interface Context {
    CoreHwmBudgetSnapshot coreHwmBudgetSnapshot();
    void resetCoreHwmBudgetMetrics();
}

public class CommonSocketOptions {
    public long sendHwm();           // non-negative outbound accounted-byte 상한을 반환한다.
    public void sendHwm(long value); // 0부터 Long.MAX_VALUE까지 Core에 전달한다.
    public long recvHwm();
    public void recvHwm(long value);
}
```

입력 우선순위는 수동 Core budget, 명시 memory limit, JVM 최대 heap hint, Core fallback
순서다. 앞의 두 값을 지정하면 JVM hint를 자동 감지하지 않는다. Binding은 hint와 Core
hard limit을 직접 결합하지 않는다. 명시 입력이 Core가 감지한 finite hard limit보다 크면
`EINVAL`에 대응하는 기존 config exception을 그대로 전달하고 clamp하지 않는다.

Core는 memory limit에 profile 비율을 정확히 한 번 적용하거나 명시 Core budget을 그대로
사용해 physical directional queue별 planned byte HWM을 계산한다. Caller가 `sendHwm(...)`이나
`recvHwm(...)`을 설정한 방향은 수동 override가 되며 이후 Auto-HWM 재계산이
그 값을 변경하지 않는다.

Java 바인딩은 queue의 message나 payload를 다시 세지 않는다. Core pipe의 실제
accounted byte가 applied HWM에 도달하면 native submit 결과가 backpressure를
나타내고, Java operation은 기존 result·timeout 계약에 따라 이를 전달한다.
`long` 값 `0`은 무제한이다. 음수 값은 HWM 입력으로 허용하지 않는다.

`monitorOpen(monitorHwmBytes, events...)`는 monitor queue의 음수가 아닌 `long` byte
값을 받는다. `0`은 Core monitor 기본값을 선택하고, 양수는 변환 없이 전달한다.
Java와 Kotlin 모두 message-count overload나 alias를 노출하지 않는다.

- `MonitorStatus` record는 native `zlink_monitor_status_t` ABI version 4와 같은 field를 제공한다.
- Planned, applied, deferred HWM과 in-flight 사용량은 non-negative `long` byte 값이며,
  더 큰 Core 값은 overflow 오류다.
- Deferred 값은 대응하는 `autoHwmDeferredSendHwmValid()` 또는 `autoHwmDeferredRecvHwmValid()`가 `true`일 때만 유효하다.
- Pending message 값은 count 진단값으로 남고 byte field와 이름을 공유하지 않는다.
- Pending byte는 `sndPendingBytes()`와 `rcvPendingBytes()`로 별도 노출한다.
- `abiVersion()`이 `3`이 아니거나 `structSize()`가 binding layout과 다르면 `UnsupportedOperationException`을 발생시킨다. 이전 monitoring layout은 받지 않는다.

`CoreHwmBudgetSnapshot`은 ABI version/size, configured/runtime/resolved memory limit,
configured/effective budget, planned/applied/manual-reserved HWM, Core queue/application/current/
peak/provisional accounted byte, completion current/peak/pending과 total messaging byte,
monitor/instance aggregate, application/completion queue count,
`outstandingApplicationLeaseCount()`, `retiredQueueCount()`, `deferredOriginCreditBytes()`,
oversize·blocked·aggregate flag, `budgetGeneration()`과 `measurementEpoch()`을 단위 변환 없이
제공한다. `applicationAccountedBytes()`와 위 세 owner-lifecycle 필드는 ABI 예약 필드이며
항상 `0`이다. Reset은 current·pending·queue count를 유지하고 두 peak를 current로
재기준화하며 epoch counter를 0으로 만든 뒤 `measurementEpoch`을 증가시킨다. Budget
snapshot ABI version/size가 맞지 않으면 `UnsupportedOperationException`이다.

Java와 Kotlin은 같은 Java method를 호출한다. 별도 Kotlin adapter나 다른 단위의 option을
추가하지 않는다. Request/reply API는 HWM 값을 인자로 받지 않으며 기존 lifetime과 ownership
계약을 유지한다.

## Receive flow state

이 바인딩은 Core의 receive-flow 상태를 `ReceiveFlowState` enum으로 노출한다.
`RUNNING(0)`, `PAUSED(1)`이며 공개 setter는 공통 socket option facade의
`receiveFlowState(ReceiveFlowState)`다. 반환형은 `void`이고 Java 에러 정책을 따른다. 0이
아닌 native 결과는 해당 `ConfigResult`를 담은 `ZlinkConfigException`으로 던지므로,
completion lane이 없는 socket은 not-supported result를 담은 `ZlinkConfigException`을
발생시킨다. 인자가 null이면 native 호출 전에 `NullPointerException`이 발생한다. 이미
유지하는 상태를 다시 설정하면 정상 반환한다.

관측 표면은 C 계약을 따르며 상수와 metric 이름은 C 계층이 확정한다. Monitor event
`SEND_FLOW_PAUSED`, `SEND_FLOW_RESUMED`, `FLOW_STATE_STALE`(`1 << 16`, `1 << 17`,
`1 << 18`, 전체 mask `0x7FFFF`), event flag `SEND_FLOW_WRITABLE`(`1 << 1`),
`FLOW_STATE_STALE_GENERATION`(`1 << 2`), `FLOW_STATE_STALE_EPOCH`(`1 << 3`), status detail
bit `FLOW_STATE`(`1 << 5`), status field 5개 `flow_paused_connections`,
`flow_pause_applied_total`, `flow_resume_applied_total`, `flow_state_stale_total`,
`flow_pause_duration_ms`를 이 언어의 이름 규칙으로 투영한다.

Flow-state frame은 Core 안에 머문다. 바인딩은 setter를 호출하고 monitor event와 snapshot
field를 읽을 뿐, flow-state frame을 직접 encode, decode, 송신 또는 수신하지 않는다.

## Error And Result Policy

Java public error는 core result 도메인의 의미를 보존하지만, native errno를
주된 사용자 API로 노출하지 않는다.

- 고정 크기 경계 값은 native 호출 전에 검증한다.
- routing id, actor id, endpoint, channel 이름, topic은 조용히 잘리지 않는다.
- `SubmitException`, `RecvException`, `RequestException`, `ConfigException`
  등 typed exception은 관련된 public result 값을 보존한다.
- native errno와 플랫폼별 error 텍스트는 진단용 세부로 등장할 수 있다.
  주된 public contract로는 등장하지 않는다.

## Spot And Actor Contract Shape

SPOT service contract는 `systems.zlink.contracts.service.spot` 아래에 둔다.

`SpotNode`가 소유하는 것:

- node lifecycle;
- service 등록;
- peer/channel 구성;
- route lookup;
- spot 생성과 lookup;
- actor 생성;
- actor route lookup;
- actor lifecycle receive;
- SPOT dispatch receive.

`Spot`은 SPOT 수준의 send/request/reply, publish, dispatch, actor operation
진입점, timer 통합에 대한 handle contract다.

Actor와 SPOT route 결과는 concrete contract 모델이다:

- `ActorRoute`는 resolve된 Actor ref, Actor node RID, 현재 Spot RID, 현재
  Spot kind를 보존한다.
- `SpotRoute`는 Spot RID, owner node RID, Spot kind를 보존한다.
- `SpotKind`는 Entry Spot과 사용자 Spot을 구분한다.
- Invalid kind는 성공한 route 결과가 아니다.

- Java는 resolve된 Actor ref를 인자로 받는 `SpotNode.sendToActor(ActorRef)`와 `SpotNode.requestToActor(ActorRef)`를 노출한다.
- send operation은 submit이 성공하면 하나 이상의 message part 소유권을 넘기고, Actor 소유자 mailbox가 인계를 받으면 완료된다.
- request operation은 submit이 성공하면 요청 part의 소유권을 넘기고, Actor handler가 만든 reply part를 전달한다.
- Java는 제거된 Discovery route table이나 resolver API를 compatibility helper로 되살리면 안 된다.

## Spot Get-Or-Create

Java는 `SpotNode.getOrCreateSpot(RoutingId)`를 노출한다.
`zlink_spot_node_spot_get_or_new(...)`에 직접 매핑되며, `spotLookup`과
`createSpot`을 조합하여 구현하지 않는다.

이 메서드는 호출자가 소유하는 `Spot` contract와 `created` boolean을 담은
concrete result를 반환한다. `created`는 해당 logical spot을 생성한 호출에 한해서
`true`다.

## Performance Policy

핫 패스는 reflection, dynamic method lookup, classpath scanning, 회피 가능한
할당, 회피 가능한 buffer 복사, 숨겨진 wait, sleep, busy wait, 넓은 lock, thread
join을 사용하지 않는다.

Callback stub과 method-handle 설정은 메시지당 처리 루프가 아니라 등록 시점에
이루어진다.

Native bridge 코드는 Java 값을 core receive substrate에서 직접 materialize해야
한다. Public contract 코드에는 raw native receive 루프가 포함되지 않는다.

Perf, 샘플, 테스트는 export된 public contract 패키지만 사용한다.

## Refactor Workflow

Java 바인딩을 정렬할 때 다음 순서를 사용한다:

1. `systems.zlink.contracts.*` 아래에 public resource interface를 정의한다.
2. 값/모델/result/exception 타입은 해당 contract 카테고리에서 concrete로
   유지한다.
3. native-backed concrete resource 클래스를 `systems.zlink.runtime.*`으로
   이동하고 `NativeContext`나 `NativeRouterSocket`처럼 구현 지향 이름으로
   바꾼다.
4. runtime 클래스가 contract interface를 구현하게 만든다.
5. factory 진입점을 public contract 타입으로 옮기고 contract interface를
   반환하게 한다.
6. native-backed resource의 직접 public constructor를 제거한다.
7. native handle, Panama/JNI 호출, callback trampoline, Core completion, marshalling
   helper, part loop를 runtime/nativeapi 또는 runtime support 클래스로 옮긴다.
8. 샘플, perf, 테스트, 문서 예시를 `systems.zlink.contracts.*`만 import하도록
   업데이트한다.
9. 옛 직접-concrete 형태를 보존하는 호환 alias와 deprecated wrapper를
   제거한다.
10. JPMS export가 contract 패키지만 노출하는지 확인한다.

concrete contract resource에서 helper 클래스만 추출하는 것으로 시작하지 않는다.
그렇게 하면 구현 세부는 감춰지지만 잘못된 public resource 설계는 그대로 남는다.

## Implementation Checklist

다음 항목이 모두 참일 때에만 Java 바인딩이 정렬된 것으로 본다:

- `Context`, socket, eventing resource, SpotNode, Spot,
  Actor의 native-backed resource가 public contract interface다.
- runtime native-backed 구현이 `systems.zlink.runtime.*` 아래에 있다.
- factory 진입점이 contract interface를 반환하고 runtime 클래스 이름을 감춘다.
- runtime 패키지가 JPMS로 export되지 않는다.
- 좁게 정당화된 factory 와이어링을 제외하면 contract 파일이
  `systems.zlink.runtime.*`을 import하지 않는다.
- public signature가 native handle, Panama memory segment, native bridge 타입,
  callback userdata, Core callback, raw part loop를 언급하지 않는다.
- DTO/값/record/enum/result/exception 타입이 concrete로 유지된다.
- Operation builder가 public contract이며 staged 상태를 감춘다.
- 샘플, perf, 테스트, 애플리케이션이 `systems.zlink.contracts.*`만 import한다.
- native-backed resource의 직접 constructor가 public 생성 경로로 남아 있지
  않다.
- 호환 wrapper, 옛 alias, deprecated 중복 operation 이름이 남아 있지 않다.
- public contract 패키지와 파일 레이아웃이 이 문서의 카테고리 map과 일치한다.

## Verification

리팩터 이후 `bindings/java/`에서 verification을 수행한다.

필수 baseline:

- `./gradlew build`
- `./tests/run_tests.sh`

생성 경로, public 예시, resource lifecycle이 바뀌면 샘플 verification을
실행한다:

- `./samples/run_samples.sh`

send, receive, request, poller, timer, service, 핫 패스 동작이 바뀌면 perf
smoke gate를 실행한다:

- `./perf/single/run_benchmarks.sh`
- `./perf/multi/run_benchmarks.sh`

필수 구조 검색:

```sh
rg -n "exports systems\\.zlink\\.runtime" src/main/java/module-info.java
rg -n "import systems\\.zlink\\.runtime\\." src/test samples perf -g'*.java'
rg -n "import systems\\.zlink\\.runtime\\." src/main/java/systems/zlink/contracts -g'*.java'
rg -n "java\\.lang\\.foreign|MemorySegment|Native[A-Za-z]*|RequestProgressPump" \
  src/main/java/systems/zlink/contracts -g'*.java'
```

앞의 세 검색은 public surface 누수를 반환하지 않아야 한다. 마지막 검색은 리뷰
이후 의도적으로 concrete로 둔 값 내부만 반환할 수 있다. public resource
interface나 operation contract가 native bridge 세부에 의존하는 결과를 보여서는
안 된다.

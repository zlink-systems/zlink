<!-- framework-adapter-nav:start -->
[문서 목록](../README.ko.md) | [Runtime Lifecycle](../../common/internals/README.ko.md) | [Regression Matrix](regression-test-matrix.ko.md)
<!-- framework-adapter-nav:end -->

# Java Backend Dependency Policy

## 1. 목적

Java/Kotlin framework는 `bindings/java`의 exported public raw socket API만 transport 경계로 사용한다.
Framework service runtime은 Java와 Kotlin이 공유하며 binding 구현 객체와 Core service object를 숨긴다.

## 2. 원칙

- framework public contract가 우선이다.
- public API에 binding concrete type을 직접 노출하지 않는다.
- native handle, socket object, monitor loop, registry runtime object는 adapter
  안에 숨긴다.
- `RoutingId`, `Message`, `SendFlags`처럼 언어 중립 의미가 있는 값 타입만 public
  contract에 남길 수 있다.
- framework는 Java binding의 exported public raw socket API만 호출한다.
- `runtime.nativeapi`, package-private 구현, JNI symbol과 Core private symbol은 호출하지 않는다.
- MeshNode, Spot, Actor, STREAM session과 maintenance 상태 기계는 JVM service runtime이 소유한다.

## 3. Adapter 계약

binding 의존은 `systems.zlink.framework.runtime.backend`의 internal port 한 묶음으로 격리한다.
이 port를 public raw binding 위에 구현한 adapter가 JVM transport 구현이다. Port와 adapter는 application
public interface가 아니며 Java module export에 포함하지 않는다.

| port interface (`systems.zlink.framework.runtime.backend`) | 역할 | Java backend 구현 대상 (`bindings/java`) |
|-----------------------------------------------|------|------------------------------------------|
| `ZLinkBackendAdapterFactory` | 정확히 5개 adapter를 만드는 factory | `ZLinkJavaBackendAdapterFactory` |
| `ZLinkChannelBackendAdapter` | dealer/router/publisher/subscriber socket wrapping, send/request, receive pump | dealer/router/publisher/subscriber socket |
| `ZLinkSpotBackendAdapter` | `SpotNode`/`Spot` wrapping(SpotNode 생성) | spotNode/spot |
| `ZLinkStreamBackendAdapter` | stream socket wrapping, session attach, frame send/reply | stream socket |
| `ZLinkMonitoringBackendAdapter` | socket/discovery/registry/spot event source를 framework typed event로 변환 | socket monitor/discovery 이벤트 |

factory는 `createChannelAdapter`, `createSpotAdapter`, `createStreamAdapter`,

### 3.1 Java binding wrapper 목록

`.NET` 의 `Runtime/Backend/DotNet/Wrappers/` 에는 binding 객체를 framework 안쪽으로
숨기는 wrapper들이 있다. Java adapter는 같은 wrapper들을 `bindings/java` 위에
1:1로 구현해야 한다.

- context wrapper
- dealer socket wrapper
- router socket wrapper
- publisher socket wrapper
- subscriber socket wrapper
- discovery wrapper
- spotNode wrapper
- spot wrapper
- stream socket wrapper
- registry wrapper
- registryQueryClient wrapper
- socket monitor wrapper

이 wrapper들의 생성은 오직 adapter 내부에서만 일어난다. wrapper가 감싸는 binding
concrete type은 public surface에 새어 나오지 않는다.

adapter는 framework internal package에 둔다. 사용자 guide와 sample은 adapter 타입을
직접 보여 주지 않는다.

Port의 정확한 시그니처는 JVM implementation internals가 소유한다. 다른 runtime의 adapter 구조를 JVM에
복사해 public abstraction으로 만들지 않는다.

## 4. Public API에 새면 안 되는 것

허용 primitive(`RoutingId`, `Message`, `SendFlags`)를 제외하고 아래 binding
concrete type과 객체 모델은 framework public contract에 직접 노출하지 않는다.

- binding `Context`
- dealer/router/publisher/subscriber socket, stream socket
- binding `Discovery`
- binding `SpotNode`, `Spot`
- native receive loop, monitor handle, timer handle
- internal frame encoder/decoder concrete type

필요한 diagnostic 값은 framework DTO로 재해석해서 노출한다. native enum이나 raw
status는 꼭 필요한 경우 optional detail로만 둔다.

## 5. 검증 기준

| 테스트 | 확인 기준 |
|--------|-----------|
| public surface backend leakage | 허용한 값 타입을 제외하고 binding concrete type이 public API에 없다 |
| backend factory wrappers | factory가 channel, spot, stream, registry, monitoring adapter 5종을 모두 만들어 내고, wrapper 생성이 adapter 내부에 머문다 |
| public raw binding only | package-private, reflection, JNI와 Core private symbol 직접 호출이 없다 |
| adapter-only transport construction | raw socket과 monitor 생성은 adapter 내부에서만 일어난다 |
| no Core service model | Core MeshNode, Spot, Actor, session service type이 JVM runtime dependency와 public ABI에 없다 |

---
<!-- framework-adapter-nav:bottom:start -->
[문서 목록](../README.ko.md) | [Runtime Lifecycle](../../common/internals/README.ko.md) | [Regression Matrix](regression-test-matrix.ko.md)
<!-- framework-adapter-nav:bottom:end -->

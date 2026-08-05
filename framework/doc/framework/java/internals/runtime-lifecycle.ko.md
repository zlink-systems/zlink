<!-- framework-adapter-nav:start -->
[문서 목록](../README.ko.md) | [다음: Regression Test Matrix](regression-test-matrix.ko.md)
<!-- framework-adapter-nav:end -->

[Java 문서](../README.ko.md) | [Kotlin 문서](../../kotlin/README.ko.md) | [Backend Policy](backend-dependency-policy.ko.md)

# Java/Kotlin Framework Runtime Lifecycle

이 문서는 Java와 Kotlin이 공유하는 Spring lifecycle과 내부 runtime 소유권을 설명한다. 사용자가 관찰하는
validation, timeout, cancellation과 reconnect 계약은 각 기능 spec이 소유한다. 네 언어가 공유하는 runtime
구조는 [공통 내부 구조](../../common/internals/README.ko.md)를 따른다.

## 1. 시작 순서

1. Spring auto-configuration이 options와 handler scanner 결과로 registration을 만든다.
2. registration validator가 channel, Spot, stream과 handler 조합을 검사한다.
3. Starter의 non-public bean assembly는 `ZLinkFrameworkRuntime`과 같은 implementation package에 위치한다.
   이 assembly가 package-private bootstrap을 호출해 `PREPARING` 상태의 facade bean을 하나 만든다. 이 단계에서는
   socket, discovery loop와 application worker를 시작하지 않는다.
4. Runtime은 RouteMesh, ClientServer와 fanout monitoring view를 각각 한 번 만들고 소유한다.
   `routeMeshRuntime()`, `clientServerRuntime()`과 `fanoutRuntime()`은 runtime 수명 동안 같은 객체를 반환한다.
5. Bean assembly는 public client와 세 topology view를 runtime에서 가져와 singleton bean으로 등록한다.
   Topology bean은 accessor가 반환한 객체를 그대로 사용하며 별도 adapter를 만들지 않는다.
6. 같은 implementation package의 non-public `SmartLifecycle` adapter가 runtime 참조를 유지한다. Adapter는
   package-private start boundary를 호출하며 facade를 만들거나 교체하지 않는다.
7. `SmartLifecycle.start()`가 runtime의 start boundary를 한 번 호출한다.
8. Runtime은 Java binding의 public raw socket API를 사용하는 backend adapter와 JVM service context를 만들고
   location, channel, route, Spot, stream, monitoring을 순서대로 시작한다.

Spring bean을 생성하는 것만으로 service runtime을 부분 시작하지 않는다. 실제 시작은 framework
`SmartLifecycle`이 소유하며, 시작 도중 실패하면 이미 만든 자원을 역순으로 정리한다. Application은 안정된
facade bean만 주입받는다. Runtime은 시작을 마치기 전까지 `PREPARING`을 보고하며 application operation이
runtime을 암묵적으로 시작하지 않는다.

Bootstrap과 start boundary는 package-private이며 public constructor, public factory 또는 lifecycle에서
runtime을 꺼내는 public accessor를 제공하지 않는다. Framework implementation은 reflection, `MethodHandles`와
private member 접근으로 이 경계를 우회하지 않는다. Auto-configuration class, bean factory method와 lifecycle
adapter의 concrete type은 application public contract에 포함하지 않는다.

## 2. 종료 순서

Spring `SmartLifecycle` 종료는 `ZLinkFrameworkRuntime.shutdown()`을 호출한다. 운영 maintenance는 `retire()`를
호출한다. Runtime은 새 dispatch 진입을 막고 accepted work, STREAM barrier, monitoring, Spot, route, stream,
channel, location, backend context 순서로 정리한다. pending completion과 coroutine continuation은 각 runtime
소유자가 완료하거나 실패시킨다. JVM thread와 coroutine dispatcher를 blocking wait로 점유하지 않는다.

`retire()`와 `shutdown()`은 host 전체를 대상으로 한다. Deprecated `drain()`과 `awaitDrained()`는 같은 host
`shutdown()` operation을 사용하는 compatibility facade다. MeshName을 받는 partial termination operation은
없으며, 하나의 topology만 별도로 종료하지 않는다.

## 3. Java/Kotlin 공유 경계

Kotlin handler는 Java runtime의 registration과 실행 queue를 공유한다. `suspend` continuation과 coroutine
context를 연결하는 wrapper만 Kotlin 전용이며, 별도 service runtime이나 lifecycle을 만들지 않는다. 공유
runtime의 RouteMesh·ClientServer probe와 Fanout liveness 기준은 공통 runtime monitoring spec과 현재 binding
package 설정을 따른다.

## 4. 회귀 테스트

| 테스트 케이스 | 확인 기준 |
|---------------|-----------|
| `HostTest.host_startsAndStops_frameworkRuntimeContext` | Spring lifecycle과 framework context 생성·정리가 함께 동작한다. |
| `ZLinkFrameworkAutoConfigurationTest.autoConfigurationStartsFrameworkLifecycleAndExposesClientBean` | auto-configuration이 lifecycle과 public client bean을 연결한다. |
| `ZLinkFrameworkAutoConfigurationTest.exposesSingleRuntimeAndTopologyRuntimeBeans` | `ZLinkFrameworkRuntime`과 세 topology view bean을 singleton으로 제공하고, 각 bean이 facade accessor의 반환값과 `assertSame`으로 일치한다. |
| `HostTest.smartLifecycleStartsAndStopsTheSameFrameworkRuntimeBean` | `SmartLifecycle` adapter가 주입받은 runtime bean을 교체하지 않고 start·shutdown한다. |
| `HostTest.beanCreationDoesNotStartRuntime` | Bean 생성은 runtime을 `PREPARING`으로 구성하지만 socket, discovery loop와 worker를 시작하지 않는다. |
| `ZLinkAsyncSubmitterTest.close_failsPendingItems` | runtime 종료가 pending submit을 남겨 두지 않는다. |
| `KotlinSuspendAnnotationHandlerTest.kotlinSuspendAnnotationCancellationCompletesJavaStageExceptionally` | Kotlin cancellation이 공유 Java completion에 전달된다. |

---
<!-- framework-adapter-nav:bottom:start -->
[문서 목록](../README.ko.md) | [다음: Regression Test Matrix](regression-test-matrix.ko.md)
<!-- framework-adapter-nav:bottom:end -->

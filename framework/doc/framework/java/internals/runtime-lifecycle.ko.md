<!-- framework-adapter-nav:start -->
[문서 목록](../README.ko.md) | [다음: Regression Test Matrix](regression-test-matrix.ko.md)
<!-- framework-adapter-nav:end -->

[Java 문서](../README.ko.md) | [Kotlin 문서](../../kotlin/README.ko.md) | [Backend Policy](backend-dependency-policy.ko.md)

# Java/Kotlin Framework Runtime Lifecycle

이 문서는 Java와 Kotlin이 공유하는 Spring lifecycle과 내부 runtime 소유권을 설명한다. 사용자가 관찰하는
validation, timeout, cancellation과 reconnect 계약은 각 기능 spec이 소유한다. 네 언어가 공유하는 runtime
구조는 [공통 내부 구조](../../common/spec/README.ko.md)를 따른다.

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

Java Framework가 생성한 binding socket은 종료 전에 reply 제출이 수락된 응답을 전송할 수 있도록 `linger`를
30초로 설정한다. 이 값은 Application option이 아니며 `ZLinkJavaSocketOptions`가 Framework 소유 raw socket을
만들 때 적용한다. `linger`는 socket close 뒤 native transport가 아직 보유한 outbound data를 정리할 수 있는
시간을 제한한다.

이 설정은 handler 완료나 remote runtime의 수신 확인을 대신하지 않는다. Runtime은 먼저 새 admission을 닫고
이미 수락한 작업을 terminal result로 끝낸 뒤 socket을 닫는다. 따라서 `linger`는 shutdown 중 accepted request의
reply가 즉시 폐기되지 않도록 하는 transport 정리 조건이며, `reply.submit()`의 수락을 remote 전달 완료로
변경하지 않는다. shutdown deadline이 만료되면 공통 shutdown 계약에 따라 남은 작업은 shutdown 결과로 끝날 수
있다.

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
| `ChannelEgressRouting/CH-E2E-04B` | ClientServer server의 shutdown 이후에도 shutdown 전에 수락한 request가 reply로 끝나고, 새 request는 다른 ready server로 전달된다. |

## 5. Native runtime package 동기화

Java binding과 Core package의 release version은 `0.10.0`으로 일치해야 한다. Java binding을
검증하거나 배포할 때는 `/home/hep7/project/kairos/zlink/.artifacts/wsl/install/zlink-core/0.10.0`의
Core package를 bridge 입력으로 지정하고, 같은 provenance manifest와 native runtime을 사용한다.

Java binding은 test classpath의 `ZLINK_LIBRARY_PATH`가 가리키는 native runtime을 먼저 확인한다.
이 값이 지정되지 않으면 source resource에 남아 있는 개발용 native 파일이 선택될 수 있다. Core를
수정한 뒤에는 local package를 다시 만들고 source resource를 동기화하거나 `ZLINK_LIBRARY_PATH`를
검증한 runtime으로 지정해야 한다. 그렇지 않으면 Java test가 수정 전 native binary를 실행하여
heap corruption을 재현하거나 수정 결과를 숨길 수 있다.

현재 poller 수명 수정은 blocking wait 중에는 `operation_sync`를 점유하지 않고, 등록 변경만
`wait_active`로 차단한다. 따라서 readiness event 변환 중 callback이 같은 poller API에 재진입해도
non-recursive mutex deadlock이 발생하지 않는다. poller를 삭제할 때는 먼저 native registration을
분리하고 ownership을 해제한다. 등록된 socket을 close하면 socket lifetime pin이 registration
remove까지 유지되고, poller는 `POLLERR`를 반환한다. Java `NativePoller.close()`는 wait가 끝날
때까지 native destroy를 예약한다. 수정 결과는 Core package verification, Java binding contract
test, 그리고 `ZLINK_LIBRARY_PATH`를 지정한 반복 실행으로 확인한다.

이번 heap corruption의 직접 원인은 Core monitor event에 추가된 diagnostic tail을 Java
`MONITOR_EVENT_LAYOUT`이 할당하지 않은 상태에서 Core가 전체 구조체를 기록한 것이다. Java layout에
`connection_id`, transport pair 식별자, transport lane과 event flags를 추가해 Core 구조체 전체
크기 816 bytes를 확보했다. 현재 Java public `MonitorEvent`가 이 diagnostic 값을 노출하지 않더라도
수신 buffer는 native public layout 전체를 수용해야 한다. `NativeLayoutsTest`가 이 크기를 고정하고,
`MonitorBehaviorContractTest`가 실제 blocking receive를 검증한다.

---
<!-- framework-adapter-nav:bottom:start -->
[문서 목록](../README.ko.md) | [다음: Regression Test Matrix](regression-test-matrix.ko.md)
Location object query는 authority page 내부의 마지막 검사 위치를 opaque continuation token에
함께 보존한다. 필터가 앞부분에서 여러 항목을 제외하거나 한 page가 1,000개 경계에서
끝나도 다음 요청이 항목을 건너뛰지 않는다. 반환 page는 JSON 표현의 encoded size를
계산해 4 MiB를 넘기지 않으며, 단일 항목이 이 제한을 넘으면 일부 결과를 성공으로 반환하지
않고 실패한다. `findSpotLocation`은 user Spot과 Instance Spot의 authority row를 모두
조회한다.

<!-- framework-adapter-nav:bottom:end -->

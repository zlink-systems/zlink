# Java/Kotlin E2E location query boundary

이 문서는 Java/Kotlin E2E fixture가 Framework의 location 기능을 연결할 때 지켜야 하는
구현 경계를 설명한다. E2E 시나리오의 공개 계약은
`framework/doc/framework/common/e2e/`와 관련 Framework spec이 소유한다.

## Store와 runtime query의 책임 구분

Location Store는 Framework runtime이 topology를 기록하고 읽는 provider contract다. E2E가
Store 장애나 지연을 만들기 위해 Store를 감싸야 하는 경우에도 public
`ZLinkLocationStore`의 `read`, `write`, `scan` 세 연산만 지연시킨다. runtime 내부 repository,
descriptor, lease 구현을 직접 구현하거나 호출하지 않는다.

Provider가 등록한 topology를 검증하는 HTTP endpoint는 Store record를 직접 읽지 않는다.
Framework runtime query가 반환하는 `ZLinkLocationTopologyEntry` page를 사용한다. Runtime query는
Framework runtime이 시작된 뒤 endpoint 요청 처리 시점에 얻는다. Spring bean 생성 시점에 query
snapshot을 요청하면 runtime이 아직 시작되지 않아 정상적인 lifecycle 순서를 위반한다.

## Topology 상태와 ready target

`listTopology` 결과에는 `READY` 외에도 `DISCOVERED`, `CONNECTING`, `LOST`, `ERROR`, `STOPPED`
상태가 포함될 수 있다. 따라서 E2E fixture는 topology row의 존재와 request target 선택을
같은 의미로 취급하지 않는다.

`LOST` provider가 topology query에 잠시 남아 있어도 Framework는 이를 ready target으로 선택하지
않아야 한다. E2E client가 ready provider 집합을 계산할 때는 topology entry의 `state == READY`
조건을 사용하고, `LOST` row가 제거되는지 여부를 성공 조건으로 사용하지 않는다.

## RouteMesh routing identity

RouteMesh channel server는 MeshNode의 routing identity를 사용한다. ClientServer channel server
builder에는 routing identity를 설정하는 public method가 없으므로, provider RID가 시나리오의
identity 계약에 포함된 경우 RouteMesh를 만들고 `setRoutingIdPrefix`로 identity를 설정한 뒤
channel server를 등록한다. ClientServer builder에 없는 method를 호출하거나 Framework 내부
구현을 통해 RID를 주입하지 않는다.

## Kotlin handler context

Kotlin suspending request handler의 context는 Framework가 제공하는
`ZLinkMessageContext`다. 존재하지 않는 별도 request context type을 만들거나 이전 API 이름을
호출하지 않는다. Java와 Kotlin fixture는 같은 request/reply 의미와 application evidence를
사용해야 하며, 언어 차이는 handler 표현 방식에 한정한다.

이 경계를 지키면 E2E 실패를 topology 계약의 문제, fixture의 상태 해석 문제, 또는 runtime
lifecycle 문제로 분리해 판단할 수 있다.

## StoreFailure SF-B2 순서와 실패 분류

공통 SF-B2는 Provider A만 ready인 상태에서 Store 장애를 failure grace보다 오래 유지한 뒤
Provider B를 시작하도록 정의한다. 장애 중에는 A request가 계속 성공하고 B가 ready target에
추가되지 않아야 하며, Store 복구 뒤 B가 current target set에 들어가야 한다. 따라서 runner가
B를 장애 전에 시작하면 시나리오 구현과 공통 절차 사이에 gap이 생긴다.

Kotlin fixture는 이 순서를 따르도록 runner를 구성한다. Framework runtime은 초기 owner lease
claim이 일시적으로 실패해도 heartbeat 주기로 claim을 재시도하고, lease generation이 바뀌면
owner-scoped descriptor를 새 generation으로 다시 게시해야 한다. 이 동작을 구현한 뒤 SF-D2는
장애 중 A traffic 유지, A 재게시, 종료된 B 제외까지 통과했다. SF-B2 recovery에서 B가
`READY` target으로 수렴하는 검증은 추가 확인 중이며, 통과하기 전에는 완료로 기록하지 않는다.

SF-D2는 긴 Store 장애 중 B가 종료된 뒤 A가 current lifecycle을 다시 게시하고, 복구 후 A만
`READY` target으로 남아야 한다. 장애 전 peer-ready evidence가 있어도 복구 후 topology query에서
A row가 사라지면 이는 fixture의 기대값 문제가 아니라 location runtime의 lease 재게시 또는
reconcile 경계 문제로 분류한다. B가 제외되었는지만으로 SF-D2를 통과시키지 않는다.

## Local readiness 진단

기본 local readiness window는 공통 E2E 문서의 3초 기준을 사용한다. JVM startup이 느린 진단
환경에서는 `ZLINK_KOTLIN_E2E_LOCAL_READINESS_ATTEMPTS`와
`ZLINK_KOTLIN_E2E_LOCAL_READINESS_POLL_SECONDS`로 bounded window만 일시적으로 늘릴 수 있다.
이 값은 시나리오의 route settle 또는 application evidence wait를 대신하지 않으며, readiness
확인에 성공했다는 이유만으로 시나리오를 통과시키지 않는다.

SF-B2에서 확인한 recovery 갭은 stale owner의 MeshNode descriptor가 새 lease generation의
`NEW_CLAIM`을 막아 `api-a`가 `LOST`로 남는 문제였다. Framework는 이전 owner token으로
stale descriptor를 먼저 제거한 뒤 새 generation으로 다시 게시하도록 수정했다. Framework와
`locations-redis`를 `0.10.0` local package로 재배포하고, 새 build/cache에서 실행한 SF-B2와
SF-D2가 각각 공통 시나리오의 outage·recovery 조건을 통과했다.

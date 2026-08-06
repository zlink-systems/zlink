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

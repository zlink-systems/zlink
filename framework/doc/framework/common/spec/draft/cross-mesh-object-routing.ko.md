# 구현 전 초안: Cross-Mesh 객체 라우팅

> 이 문서는 구현 전 설계 초안이다. 현재 공개 계약이 아니며, 이 문서만으로
> public API나 동작을 추가하지 않는다.

## 문제 정의

공통 Framework 계약은 ActorId와 SpotId를 Location Store의 전역 namespace에서
식별한다. 이 규칙은 객체 identity와 authority 수렴을 정의하지만, 서로 다른
`MeshName` 사이에서 요청과 응답을 전달하는 transport 경계까지 정의하지 않는다.

현재 RouteMesh는 local `MeshName`과 다른 peer descriptor를 admission하지 않는다.
이 격리는 bounded context와 failure domain을 보존하기 위한 규칙이므로, global
object identity를 이유로 완화하지 않는다.

따라서 global identity와 cross-Mesh message delivery를 하나의 기능으로 해석하면
다음 계약이 비어 있다.

- target Mesh를 discovery 결과에서 어떻게 선택하는가
- cross-Mesh transport 또는 relay의 ownership은 어느 runtime에 있는가
- readiness와 reconnect를 어떤 public 결과로 표현하는가
- authorization, hop limit, loop 방지와 deadline을 어디에서 적용하는가
- Actor와 User Spot의 create, find, request, relocation을 같은 규칙으로 보장하는가
- backpressure와 allocation 비용을 local RouteMesh와 어떻게 비교하는가

## 검토할 대안

### 대안 A: 호출 runtime이 target Mesh별 RouteMesh를 명시적으로 등록

호출 runtime이 대상 Mesh마다 별도의 public RouteMesh connection을 구성하고,
Location Store의 target Mesh를 해당 connection에 매핑한다. 각 connection은 하나의
MeshName만 admission하므로 Core와 Framework의 topology 격리를 유지할 수 있다.

이 방식은 구현 경계가 단순하지만, 호출자가 여러 Mesh의 endpoint, readiness,
reconnect와 오류를 직접 관리해야 한다. 동일 객체 요청이 여러 connection을 거칠
때의 retry와 duplicate 방지도 별도로 정의해야 한다.

### 대안 B: Framework 소유 relay 또는 gateway

Framework가 명시적인 relay aggregate를 소유하고, local RouteMesh와 target Mesh의
RouteMesh 사이에서 typed request를 전달한다. 호출자는 target Mesh를 public domain
계약으로 지정하지 않고 global object operation을 요청한다.

이 방식은 호출부를 단순하게 만들지만 relay의 lifecycle owner, security boundary,
hop 제한, queue budget, p99 latency와 장애 전파를 새로 정의해야 한다. raw frame
우회나 binding-specific adapter로 구현하지 않는다.

## 설계 판정 조건

다음 조건을 정식 spec에 먼저 고정하지 않으면 두 대안 모두 구현하지 않는다.

1. cross-Mesh operation의 public result와 error kind
2. target Mesh discovery와 authority version의 일관성 규칙
3. transport ownership, readiness, reconnect와 shutdown 순서
4. Actor와 User Spot의 identity, generation, relocation 처리
5. authorization, loop 방지, deadline, backpressure와 observability
6. local RouteMesh 대비 allocation, copy, lock contention, p99 latency 기준선

RM-A7 E2E는 위 조건을 결정하기 위한 검증 입력으로만 사용한다. 다른 언어의
구현이나 E2E fixture에 기능이 있다는 사실만으로 C++ public API를 추가하지 않는다.

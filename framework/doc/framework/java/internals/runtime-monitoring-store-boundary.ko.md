# Runtime monitoring과 Location Store 경계

Java Framework의 public runtime snapshot은 Location Store 응답 지연 때문에 호출자를
무기한 대기시키지 않는다. Store descriptor와 status는 placement limit, capability, topology
상태를 보완하는 입력이지만, monitoring 호출의 완료를 결정하는 외부 동기화 지점으로 사용하지
않는다.

## 조회 규칙

Runtime snapshot이 Store descriptor를 조회할 때는 bounded timeout을 적용한다. 제한 시간 안에
응답이 도착하지 않으면 runtime registration에 보관된 limit과 capability를 사용해 projection을
만들고, topology 상태에는 Location Store 불가 상태를 반영한다. Store 조회 실패가 public
snapshot 요청의 timeout으로 전파되지 않아야 한다.

Store descriptor의 `active` 값은 Framework process가 현재 보유한 Actor와 User Spot 수의
최신 값으로 간주하지 않는다. Java runtime owner가 가진 live count를 snapshot projection에
반영하고, descriptor에서 받은 limit, reserved, capability, placement weight는 별도로
유지한다. 이 결합은 public placement contract를 한 곳에서 구성하기 위한 것이다.

## User Spot capacity

User Spot creation은 Location Store reservation과 대상 MeshNode의 admission을 모두 거친다.
대상 선택 단계에서 descriptor의 전체 Spot capacity가 남아 있으면 후보로 선택할 수 있지만,
Spot type별 세부 capacity는 authoritative reservation 결과로 확정한다. reservation 또는
대상 admission이 capacity 초과를 반환하면 같은 요청을 readiness 대기로 재시도하지 않고
`CAPACITY_EXCEEDED` terminal 결과로 변환한다.

이 규칙은 descriptor의 세부 active 값이 갱신되는 사이에도 capacity 초과 요청이 무한히
재시도되는 문제를 막는다. pending 상태의 생성과 reservation 종료는 Location Store의
atomic reservation 결과와 terminal reconciliation을 기준으로 관리한다.

관련 공통 계약은
[`Runtime monitoring`](../../common/spec/server/24-runtime-monitoring.ko.md)과
[`MeshNode`](../../common/spec/server/13-mesh-node.ko.md)이다. Java public interface는
[`Java monitoring interface`](../../common/spec/server/languages/java/interfaces/monitoring.ko.md)를
따른다.

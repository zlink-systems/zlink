# C++ Runtime Monitoring과 Location Lifecycle

이 문서는 C++ framework가 public `route_mesh_runtime_t`와 Location Store를
결합하는 내부 구조를 설명한다. Application이 사용하는 status 모델과 binding의
monitor event 모델은 같은 객체가 아니므로, C++ runtime이 두 모델 사이의 의미를
변환한다.

## 1. 책임 경계

`route_mesh_runtime_service_t`는 MeshNode의 transport 상태, Location Store 조회
결과와 application claim 상태를 하나의 `mesh_node_snapshot_t`로 투영한다. 이
snapshot은 다음 정보만 public status로 노출한다.

- MeshNode state와 `is_ready`
- peer의 Routing ID와 peer state
- Channel별 ready target 수
- placement availability와 active Actor·Spot 수
- 단조 증가하는 snapshot `sequence`

Endpoint, descriptor revision, owner lease, connection generation과 native monitor
event DTO는 public snapshot에 넣지 않는다. 이 제한은 binding type이 Framework
domain model로 퍼지는 것을 막고, transport identity와 service readiness를 혼동하지
않게 한다.

## 2. Snapshot과 observer의 실행 순서

Transport topology 변경, Location descriptor polling, host lifecycle 변경이
발생하면 runtime service는 다음 순서로 처리한다.

1. 현재 node와 Location Store 상태를 읽어 snapshot을 만든다.
2. MeshName별 `sequence`를 증가시킨다.
3. hub에 최신 snapshot을 보관한다.
4. observer별 bounded queue에 snapshot을 넣는다.
5. observer callback은 queue를 소비하는 실행 owner에서 호출한다.

observer callback의 예외는 다른 observer, transport lifecycle 또는 application
dispatch로 전파하지 않는다. 느린 observer는 전체 runtime을 막지 않으며, queue가
압박을 받으면 최신 snapshot으로 따라잡는다. 따라서 callback은 snapshot을 다시
만들거나 binding monitor event를 직접 해석하지 않는다.

## 3. Structured log의 peer event

`zlink.runtime.mesh_node.peer_changed`는 public snapshot의 peer state 변화에서
파생한다. C++ RuntimeMonitoring E2E service는 public `observe()` 결과를 비교해
ready 집합에 새로 들어온 peer에는 `ConnectionReady`, 빠진 peer에는
`Disconnected`를 기록한다. 각 record에는 MeshName, Routing ID, snapshot
sequence와 현재 topology state가 포함된다.

이 기록은 내부 descriptor generation을 추가로 공개하지 않는다. Crash replacement
검증은 owner lease 만료 뒤 이전 peer가 ready 목록에서 빠지고 새 process의 동일
RID가 다시 ready가 되는지, 그리고 그 직후 request가 한 번 처리되는지를 status와
request 결과로 확인한다.

## 4. Binding monitor의 physical connection identity

binding public `monitor_event_t`는 native Core가 제공하는
`connection_id`, `transport_pair_id`, `transport_pair_generation`,
`transport_lane`, `flags`를 보존한다. Framework mesh owner는 readiness와
disconnect 대상을 찾을 때 `value`가 아니라 `connection_id`를 사용한다.

`value`는 event별 부가 값이며 물리 transport 시도의 identity가 아니다. 이를
혼용하면 한 peer의 이전 connection이 새 connection을 제거하거나, 정상적인
replacement가 stale connection으로 분류될 수 있다. 이 변환은 binding public
field만 사용하며 native private storage나 reflection에 의존하지 않는다.

## 5. Crash replacement와 owner lease

정상 종료는 owner cleanup이 descriptor와 lease를 제거할 수 있지만, 강제 종료는
그 작업을 수행하지 못한다. 따라서 replacement가 같은 role과 RID를 즉시 claim하면
기존 owner lease가 유효한 동안 `rejected_conflict`를 받는다.

C++ RuntimeMonitoring E2E runner는 peer가 not-ready가 된 뒤에도 replacement를
바로 시작하지 않는다. 설정된 owner lease TTL과 fencing margin을 기다린 후
replacement를 시작한다. 만료 전 claim을 허용하는 takeover 우회는 stale owner가
현재 descriptor를 덮어쓸 수 있으므로 사용하지 않는다.

## 6. Logging provider의 격리

Framework logging callback은 관측 경계다. callback sink가 예외를 던져도 logger는
해당 sink 호출을 끝내고 dispatch 및 host lifecycle을 계속 수행한다. 여러 sink는
각각 독립적으로 호출되며 한 sink의 실패가 다른 sink의 호출을 중단하지 않는다.
이 규칙은 monitoring callback을 검증하는 throwing E2E profile에서도 동일하게
적용된다.

## 7. 검증 위치

- binding monitor contract: `bindings/cpp/tests/contract/test_cpp_contract_monitor.cpp`
- C++ RuntimeMonitoring E2E: `framework/languages/cpp/e2e/RuntimeMonitoring/run_e2e.sh`
- public snapshot contract: `framework/languages/cpp/framework/include/zlink/framework/contracts/monitoring/route_mesh_runtime.hpp`
- runtime projection: `framework/languages/cpp/framework/src/runtime/mesh/route_mesh_runtime_service.cpp`
- monitor identity conversion: `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp`

## 8. Monitor event ABI와 callback 수명

확장된 monitor identity field는 현재 0.11.1
`zlink_socket_monitor_recv` entry point를 통해 읽는다. 이전 event prefix를
위한 별도 receive entry point나 size/version 협상은 제공하지 않는다. C++
binding의 pull 방식 monitor와 callback 경로는 모두 현재 event layout을
사용하되 Framework에는 public binding field만 전달한다.

C++ `socket_monitor_t`의 callback userdata는 movable한
`socket_monitor_t` 객체 주소가 아니라 monitor 구현이 소유하는 callback
state를 가리킨다. Monitor를 이동해도 Core가 moved-from 객체나 제거된 객체의
주소를 사용하지 않는다. Callback에서 예외가 발생하면 Core callback 경계에서
예외를 차단하고 callback depth 계산과 self-close 정리를 계속 수행한다.

## 9. 저장소 장애와 snapshot 조회

`route_mesh_runtime_service_t`의 public `snapshot()`은 Location Store를 직접 조회하지 않는다.
Location Store 조회는 runtime pump가 수행하고 mesh별 descriptor cache를 갱신한다. snapshot은
이 cache와 native topology를 결합하므로 Store 요청의 timeout이 monitoring HTTP 호출이나
observer의 현재 snapshot 조회를 막지 않는다.

Location runtime의 heartbeat가 stale owner token을 확인하면 같은 owner identity로 lease를
재획득한다. 성공하면 새 token과 Store 시간을 기록하고 `store_healthy`를 복구한다. 같은 routing
identity를 사용하는 process replacement는 이전 owner의 TTL과 fencing margin이 끝난 뒤 descriptor를
publish한다.

## 10. Descriptor cache 초기화와 Store query 부재

Runtime service는 시작할 때 descriptor cache를 한 번 채운 뒤 pump를 시작한다. 따라서 시작 직후의
`snapshot()`도 placement와 local descriptor 정보를 포함하며, 첫 polling 주기를 기다리거나
snapshot 호출마다 Store를 읽지 않는다.

`location_runtime_query_t`가 연결되지 않은 in-process projection에서는 caller가 제공한 descriptor를
완전한 입력으로 사용한다. 이 경우 health cache가 없다는 이유만으로 snapshot을 `degraded`로 낮추지
않는다. 반대로 query service가 연결된 구성에서는 cache에 기록된 Store health를 사용해 Store 장애를
`degraded`로 반영한다.

Store descriptor가 현재 transport peer 목록보다 우선하는 구성에서는 Store에 없는 peer를 public
snapshot의 ready 후보로 유지하지 않는다. 이 규칙은 Store에서 제거된 descriptor가 transport topology의
오래된 항목으로 남아 있는 동안에도 public readiness가 이전 상태를 재사용하지 않게 한다.

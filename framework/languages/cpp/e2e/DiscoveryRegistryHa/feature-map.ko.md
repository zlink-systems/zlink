# C++ DiscoveryRegistryHa (Config 6 StoreFailure) E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-6-store-failure-recovery.ko.md`

`DiscoveryRegistryHa`는 C++ build target과 runner directory의 식별자다. 이 실행 lane이 검증하는
공개 계약과 시나리오 ID는 Config 6 StoreFailure의 `SF-*`만 사용한다.

이 문서는 C++ Config 6 E2E의 현재 구현 상태를 기록한다. 실행 구성은 Redis
location store를 공유하는 provider와 consumer, scenario를 구동하는 client로 나뉜다.
열 개 client 검증은 `Client/Scenarios/sf_*_scenario.hpp`에 ID별로 분리되어 있고
`Client/main.cpp`는 설정과 dispatch만 담당한다.

| 시나리오 | 상태 | 근거 |
|----------|------|------|
| SF-A1 | 구현 | Redis location store가 정상일 때 provider 2개가 MeshNode descriptor로 보이고, consumer request가 provider에 도달한다. consumer/provider runtime status는 store와 owner lease가 healthy이고 실제 owner lease 갱신 시각과 last refresh 시각이 있음을 보여 준다. |
| SF-A2 | 구현 | C++ Redis store는 watch 없이 polling 경로로 동작한다. 초기 부재 확인 뒤 별도 `api-c`를 추가하고 polling 제한 안의 peer 반영과 실제 routing 응답을 확인한다. 정상 종료 뒤에는 같은 제한 안의 peer 제거와 후속 routing 제외를 public `/query/*`와 `/profile/request`로 검증한다. |
| SF-B1 | 구현 | Redis container process를 정지한 동안 기존 연결 request가 계속 성공하고, runtime status가 store unhealthy로 바뀐 뒤 빈 store 재기동 후 healthy로 회복된다. |
| SF-B2 | 구현 | Redis 정지 중 `api-b`를 새 channel endpoint에서 재기동한다. store failure grace를 넘길 때까지 기존 `api-a` 연결의 request만 성공하고, 빈 store 복구 뒤 새 endpoint row가 등록되어 `api-b`가 다시 요청을 처리한다. |
| SF-C1 | 구현 | provider `api-b`를 SIGABRT로 crash시키면 raw Redis descriptor record는 남지만 framework의 owner lease join이 lease 만료 뒤 live descriptor/runtime snapshot에서 제외하고, 이후 request는 survivor `api-a`로만 간다. |
| SF-C2 | 구현 | provider `api-b`가 공개 drain lifecycle로 `Draining=true`를 게시하고 실제 polling 전파 상한 동안 lease를 갱신한다. consumer는 연결을 유지한 채 신규 request 선택에서 `api-b`를 제외한다. terminal `drained` 뒤 owner row와 lease가 TTL 만료 전에 제거되고 process가 정상 종료된다. |
| SF-D1 | 구현 | 두 provider 연결을 실제 request로 준비하고 장애 전부터 복구 뒤까지 traffic을 유지한다. local row 재등록과 heartbeat 유예 뒤 status가 회복되며, 두 endpoint의 Connected/Disconnected count가 늘지 않는다. |
| SF-D2 | 구현 | 장애 전부터 지속 traffic을 흘리고 최대 성공 간격을 제한한다. Redis 정지 중 `api-b`가 crash된 뒤 `api-a` socket count는 유지되고 `api-b` Disconnected만 증가하며, owner lease join에서 dead row가 제외된다. |
| SF-D3 | 구현 | Redis process 정지·재기동 동안 runtime heartbeat 상태가 healthy → unhealthy(last error 포함) → healthy 순서로 관측된다. 장애 중에는 마지막 성공 시각을 보존하고, 복구 뒤 owner lease 갱신 시각과 last refresh 시각이 장애 전 값보다 증가하는지 확인한다. 상태 조회 자체는 store probe를 실행하지 않는다. |
| SF-E1 | 구현 | harness의 TCP proxy가 실제 Redis 응답에 300ms 지연을 주입한다. 지연된 descriptor query가 실제로 느려지는 동안 같은 consumer process의 runtime status 조회와 application request p99가 baseline 기반 budget 안에 남고, 지연 해제 뒤 request가 정상 복구되는지 검증한다. |

표준 `/profile/request`는 내부 retry 없이 5초 제한의 framework request 한 번만 실행한다. 따라서 각 scenario의 request 성공은 늦은 재시도로 복구된 결과가 아니라 해당 시점 연결의 실제 결과다.

## 검증 방법

`run_e2e.sh all`은 SF-A1부터 SF-E1까지 모든 행을 실행한다. 단일 시나리오는 해당 ID를 인자로
전달해 실행한다. runner는 Redis 장애와 process 종료가 시나리오에서 요청한 fault injection인지
구분하고, 예상하지 않은 역할 종료는 실패로 처리한다.

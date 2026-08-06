# Node.js StoreFailure E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-6-store-failure-recovery.ko.md`

| Scenario | 상태 | Node 구현 메모 |
|----------|------|----------------|
| SF-A1 | 구현 | Redis location store + provider 2개 + consumer baseline. public MeshNode runtime snapshot topology와 request/evidence를 검증한다. |
| SF-B1 | 구현 | Redis container를 정지한 동안 fail-static으로 기존 연결 request가 유지되고 runtime status가 unhealthy/lastError를 노출하는지 검증한다. |
| SF-C1 | 구현 | `api-b` SIGKILL 뒤 owner lease 만료만으로 stale MeshNode descriptor가 `/location/mesh-nodes` 성공 결과에서 제외되고 후속 request가 `api-a`로만 가는지 검증한다. |
| SF-D1 | 구현 | Redis container를 짧게 제거한 뒤 같은 endpoint에 빈 container로 재기동하고 request 전 구간 성공, runtime status recovery, descriptor/runtime snapshot recovery를 검증한다. |
| SF-D2 | 구현 | Redis 장기 중단 중 `api-b`를 SIGKILL하고 빈 store 재기동 뒤 `api-a` 재등록, `api-b` 제외, request 전 구간 성공과 `api-a` 단독 routing을 검증한다. |
| SF-A2 | 구현 | Redis location store의 `watchEnabled=false` 상태에서 provider 추가와 정상 제거가 polling interval 안에 descriptor/runtime snapshot와 request routing에 반영되는지 검증한다. |
| SF-B2 | 구현 | store 중단 뒤 `api-b`를 같은 endpoint로 재시작하고, `storeFailureGraceMs` 6000ms를 넘기는 동안 기존 `api-a` 연결의 request만 성공하며 새 `api-b` 연결과 처리 evidence가 생기지 않는지 검증한다. |
| SF-C2 | 구현 | `api-b`가 server weight를 0으로 바꾼 뒤 `Draining=true`를 게시하고, 새 request가 `api-a`로만 배정되는지 검증한다. drain은 30초 안에 `drained`로 끝나야 하며, owner row가 즉시 제거되고 Redis 저장소까지 정리한 프로세스가 강제 종료 없이 끝나야 한다. |
| SF-D3 | 구현 | Redis pause/unpause 사이클 동안 consumer public MeshNode runtime snapshot의 healthy → unhealthy/lastError → healthy/lastRefresh 전이를 검증한다. |
| SF-E1 | 구현 | Redis `CLIENT PAUSE`로 store 응답 지연을 주입하고, 같은 consumer process가 pending store read를 가진 동안 기존 channel profile request를 낮은 지연으로 계속 처리하는지 검증한다. |
| SF-B3 | 구현 | Instance Spot에 public `addTimer`를 등록하고 provider의 public location status에서 owner lease 만료를 확인한 뒤, 같은 Spot request가 실패하고 만료 시점 이후 timer evidence가 증가하지 않는지 검증한다. |
| SF-C3 | 구현 | 동일 RID replacement와 old lifecycle pause를 구성한다. Core의 same-side duplicate RID connection은 기존 current pipe를 유지하고 새 연결을 standby로 둔다. old process 재개 뒤에도 replacement가 current ready peer와 request 처리 주체로 유지되며 old evidence가 증가하지 않는지 검증한다. |
| SF-C4 | 구현 | SF-C4에서만 multi-role profile을 활성화해 한 host의 RouteMesh 2개, ClientServer와 fanout을 구성한다. host replacement 뒤 public readiness를 확인하고, 각 역할의 marker가 replacement provider 또는 consumer subscriber에서 정확히 한 번 처리되며 old provider evidence가 증가하지 않는지 검증한다. |
| SF-C5 | 미구현 | page size와 누락·중복은 검증하지만, 현재 fixture는 만료된 owner lease record를 만들지 않아 owner lease filtering을 실제로 판정하지 못한다. stale owner fixture와 public query exclusion assertion이 필요하다. |
| SF-F1 | 미구현 | 실제 cross-language object/state process fixture가 필요하다. Node-only smoke는 계약 증거가 아니다. |
| SF-F2 | 미구현 | 장기 capture/restore lease와 독립 Relocation Store 장애를 구성해야 한다. |
| SF-F3 | 미구현 | Location Store와 Relocation Store를 분리한 장애 fixture가 필요하다. |
| SF-F4 | 미구현 | object generation과 owner replacement를 public ref로 함께 검증해야 한다. |
| SF-F5 | 미구현 | creating owner crash와 bounded recovery result를 전용 Instance Spot fixture로 검증해야 한다. |
| SF-F6 | 구현 | 1,001개 Instance Spot을 준비한 뒤 첫 public page를 받은 시점에 새 Spot을 만들고 기존 Spot을 public handler의 close 동작으로 제거한다. 기존 continuation scan의 중복·page cap과 새 scan의 추가·제거 반영을 검증한다. |
| SF-F7 | 미구현 | public relocation size limit 안팎의 large state를 capture/restore해야 한다. |
| SF-F8 | 미구현 | target owner lease 만료 뒤 source 보존을 relocation fixture로 검증해야 한다. |
| SF-F9 | 구현 | SF-C3와 동일한 public channel replacement fixture에서 old provider를 pause/resume하고, replacement가 계속 current ready target로 선택되며 old lifecycle 재개 뒤에도 replacement evidence만 증가하는지 SF-F9 marker로 검증한다. |
| SF-F10 | 미구현 | 다수 accepted request와 relocation completion의 ID별 terminal을 함께 검증해야 한다. |
| SF-F11 | 미구현 | cancellation/response loss 뒤 payload isolation과 단일 terminal을 검증해야 한다. |
| SF-G1 | 미구현 | Actor·Spot·stable type의 양수 capacity, CapacityExceeded 결과, factory failure rollback과 cleanup 후 slot 재사용을 같은 fixture에서 검증해야 한다. |
| SF-G2 | 구현됨 | activation concurrency gate가 Instance factory 실행을 제한하고 public descriptor의 active projection을 갱신한다. 32개 valid create의 최종 성공과 관찰된 active 상한을 검증한다. |
| SF-G3 | 미구현 | User Spot aggregate capacity의 all-or-none 결과를 검증해야 한다. |

## 검증

- `./e2e/DiscoveryRegistryHa/run_e2e.sh SF-C1`
  - 로그: `logs/20260703-204014-50300`
  - 결과: `scenario SF-C1 passed`, `store-failure-recovery scenario result=passed`
- `./e2e/DiscoveryRegistryHa/run_e2e.sh SF-D1`
  - 로그: `logs/20260703-204208-60437`
  - 결과: `scenario SF-D1 passed`, `store-failure-recovery scenario result=passed`
- `./e2e/DiscoveryRegistryHa/run_e2e.sh SF-D2`
  - 로그: `logs/20260703-204434-71368`
  - 결과: `scenario SF-D2 passed`, `store-failure-recovery scenario result=passed`
- `./e2e/DiscoveryRegistryHa/run_e2e.sh`
  - 로그: `logs/20260703-205114-96377`
  - 결과: 현재 P0 sweep `SF-A1`·`SF-B1`·`SF-C1`·`SF-D1`·`SF-D2` 통과
- `./e2e/DiscoveryRegistryHa/run_e2e.sh SF-D3`
  - 로그: `logs/20260707-113447-1458059`
  - 결과: `scenario SF-D3 passed`, `store-failure-recovery scenario result=passed`
- `./e2e/DiscoveryRegistryHa/run_e2e.sh SF-C2`
  - 로그: `log/20260715-134600-3617212`
  - 결과: `Draining=true` 관측, drain 중 `api-a` 단독 배정, `drained`, owner row 제거와 `api-b` 자연 종료를 확인했으며 `scenario SF-C2 passed`, `store-failure-recovery scenario result=passed`

- `./e2e/DiscoveryRegistryHa/run_e2e.sh SF-B3`
  - 결과: Instance Spot baseline request와 timer evidence를 확인하고 Redis 중지 뒤 provider owner lease 만료를 관찰했다. 만료 이후 동일 Spot request는 실패하고 timer evidence는 증가하지 않았으며 `scenario SF-B3 passed`, `store-failure-recovery scenario result=passed`를 확인했다.
- `./e2e/DiscoveryRegistryHa/run_e2e.sh SF-B2`
  - 로그: `logs/20260707-113841-1472835`
  - 결과: `scenario SF-B2 passed`, `store-failure-recovery scenario result=passed`
- `./e2e/DiscoveryRegistryHa/run_e2e.sh SF-A2`
  - 로그: `logs/20260707-114016-1479743`
  - 결과: `scenario SF-A2 passed`, `store-failure-recovery scenario result=passed`
- `./e2e/DiscoveryRegistryHa/run_e2e.sh`
  - 로그: `log/20260715-134500-3612641`부터 `log/20260715-134647-3621134`까지의 scenario별 실행 로그
  - 결과: `SF-A1`·`SF-A2`·`SF-B1`·`SF-B2`·`SF-C1`·`SF-C2`·`SF-D1`·`SF-D2`·`SF-D3`·`SF-E1` 통과, `store-failure-recovery e2e result=passed`
- `./e2e/DiscoveryRegistryHa/run_e2e.sh SF-E1`
  - 로그: 최신 실행 후 기록
  - 결과: Redis 응답 지연 중 unrelated profile request 비블로킹 검증
- `./e2e/DiscoveryRegistryHa/run_e2e.sh SF-C5`
  - 로그: `log/20260806-141516-386731`
  - 결과: 1,001개 Instance Spot과 page size 1/100/1,000의 public object query, owner lease filtering을 확인했으며 `scenario SF-C5 passed`, `store-failure-recovery scenario result=passed`

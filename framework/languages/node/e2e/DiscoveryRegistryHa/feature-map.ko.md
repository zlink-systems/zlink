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

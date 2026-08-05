# Kotlin DiscoveryRegistryHa E2E feature map

이 문서는 `DiscoveryRegistryHa` lane이 담당하는 Config 6 StoreFailure 공통 시나리오 중 Kotlin E2E가
현재 검증하는 항목을 정리한다.
Kotlin 구현은 Java framework Redis location store extension을 Spring bean으로 등록하고,
consumer는 public `ZLinkClient`와 주입받은 public `ZLinkLocationRuntimeQuery`만 사용한다.

## 구현됨

- `SF-A1`: Redis location store에 provider MeshNode descriptor 2개가 보이고, consumer location runtime status가
  healthy이며, channel request가 location resolver를 통해 provider로 라우팅되는지 검증한다.
- `SF-A2`: watch 기능을 쓰지 않는 polling-only store wrapper에서 provider 추가/제거가 polling으로
  반영되고 request가 성공하는지 검증한다.
- `SF-B1`: Redis store outage 중 기존 static route로 요청이 계속 처리되고, unhealthy status와 owner
  lease failure가 public status endpoint에 드러나는지 검증한다.
- `SF-B2` (부분 구현): store failure grace를 넘는 outage 동안 기존 요청과 복구 후 descriptor·request
  경로는 확인한다. grace 초과 뒤 재시작한 provider로 신규 outbound 연결을 만들지 않는다는 단언은
  shared Java runtime 결함이 수정된 뒤 다시 검증해야 한다.
- `SF-C1`: provider crash 뒤 owner lease TTL과 polling 주기 안에서 비정상 종료된 provider descriptor가 제외되고
  survivor provider만 요청을 처리하는지 검증한다.
- `SF-C2`: graceful provider shutdown이 owner lease TTL보다 빨리 row를 제거하고 survivor request만
  남기는지 검증한다.
- `SF-D1`: 짧은 Redis outage 동안 request traffic이 멈추지 않고, 복구 뒤 MeshNode descriptor와 request가 다시
  healthy 상태로 수렴하는지 검증한다.
- `SF-D2`: 긴 Redis outage 중 provider crash가 겹쳐도 survivor request가 이어지고, 복구 뒤 비정상 종료된
  provider row 없이 survivor provider로만 후속 request가 가는지 검증한다.
- `SF-D3`: healthy, outage, recovered status transition에서 last refresh, owner lease renewal,
  watch/polling state, last error가 public status endpoint로 드러나는지 검증한다.

## 전환 필요

- `SF-E1`: Redis 연결을 끊지 않고 응답 지연만 주입한 동안 location store와 무관한 application
  request의 지연 분포와 `Snapshot(meshName)` 조회의 비블로킹을 검증해야 한다. 현재 outage runner는
  응답 지연만 발생하는 조건을 만들지 않으므로 완료 근거가 아니다.

## 포팅 구조 상태

Kotlin StoreFailure E2E는 `Shared`, `Client`, `Server/Provider`, `Server/Consumer`로 구성한다.
Provider와 Consumer는 `ZLinkRedisLocationStore`를 등록한다. Consumer는 public
`ZLinkLocationRuntimeQuery` bean을 주입받으며, HTTP endpoint `/locations/status`와
`/locations/mesh-nodes`를 통해 그 조회 결과를 client에 제공한다. runtime 객체의 별도 query accessor는
공개 계약으로 가정하지 않는다.

## 검증 방법

`run_e2e.sh all`은 SF-A1부터 SF-E1까지 지원하는 selector를 실행한다. `SF-B2`와 `SF-E1`은 표에 적은
누락을 구현하고 다시 검증한 뒤에만 완료로 바꾼다.

# Java StoreFailure E2E feature map

이 문서는 공통 E2E Config 6 `Store Failure Recovery` 계약과 Java 구현의 대응 관계를 정리한다.
검증 기준은 `framework/doc/framework/common/e2e/config-6-store-failure-recovery.ko.md`다.

## 실행 구조 상태

- `implemented`: Gradle 실행 그래프는 `Shared`, `Client`, `Server/Provider`,
  `Server/Consumer`로 구성하며, Config 6 선택자를 Client가 실행한다.
- `implemented`: provider와 consumer는 Redis location store extension을 같은 endpoint와 key
  prefix로 등록한다.
- `implemented`: consumer는 public `ZLinkLocationRuntimeQuery`를 HTTP endpoint로 노출한다.
- Config 6의 각 selector가 Java location store 경로를 실행한다. 완료 여부와 남은 단언은 아래
  시나리오별 행을 기준으로 판정한다.

## 구현됨

- `SF-A1`: Redis location store + provider 2개 + consumer 자동 연결 baseline을 검증한다.
  consumer의 public MeshNode runtime snapshot에서 provider MeshNode descriptor 2개가 보이고, store status가 healthy이며,
  consumer request가 provider 중 하나에서 처리되는지 확인한다.
- `SF-A2`: change-stamp surface를 숨긴 polling-only consumer가 `watchEnabled=false` 상태에서
  provider 추가(`api-c`)와 제거를 polling interval 안에 descriptor/runtime snapshot와 request path로 반영하는지 확인한다.
- `SF-B1`: runner가 전용 Redis 컨테이너를 pause한 동안 기존 consumer 연결의 request가 계속
  성공하고, public runtime status가 store unhealthy와 owner lease heartbeat failure를 보여준 뒤
  unpause 후 healthy로 복구되는지 확인한다.
- `SF-B2`(부분 구현): Redis outage를 store failure grace보다 길게 유지하면서 기존 연결 request가
  계속 성공하는지 확인하고, outage status와 recovery 후 provider row 복구를 public endpoint로
  검증한다. grace 초과 뒤 재시작한 provider로 새 outbound 연결을 만들지 않는다는 단언은
  E2E-JV-07의 Java runtime 결함 때문에 아직 추가하지 못했다.
- `SF-C1`: `api-b`를 SIGKILL해 row 제거 없이 종료시킨 뒤, owner lease 만료 후 public descriptor/runtime snapshot에서
  `api-b`가 제외되고 consumer request가 survivor인 `api-a`로만 빠르게 처리되는지 확인한다.
- `SF-C2`: provider HTTP `/shutdown`으로 `api-b`를 정상 종료시킨 뒤, owner lease TTL을 기다리지
  않고 public descriptor/runtime snapshot에서 `api-b`가 사라지고 request가 `api-a`로만 가는지 확인한다.
- `SF-D1`: 전용 Redis 컨테이너를 owner lease TTL보다 짧게 pause/unpause하면서 background client가
  계속 request를 보내고, 모든 request 성공과 복구 후 healthy status 및 provider row 2개를 확인한다.
- `SF-D2`: 전용 Redis 컨테이너를 owner lease TTL보다 길게 pause하고 그 동안 `api-b`를 SIGKILL한다.
  복구 뒤 정상 provider `api-a`가 재등록되어 request를 계속 처리하고, 재등록하지 못한 `api-b`만 peer
  list에서 빠지는지 확인한다.
- `SF-D3`: 전용 Redis 컨테이너를 pause/unpause하면서 public runtime status가 healthy 상태에서
  last refresh와 owner lease 갱신 시간을 노출하고, outage 중에는 store/owner lease unhealthy와
  last error를 보이며, 복구 뒤 last refresh 시간이 전진하는지 확인한다.
- `SF-E1`: consumer의 Redis location store wrapper가 public descriptor query에 응답 지연을 주입한다.
  지연된 descriptor query가 실제로 지연되는 동안 같은 consumer의 일반 request p99가 baseline 기반
  budget 안에 머무르고, 지연 해제 뒤 follow-up request가 성공하는지 확인한다.

## 남은 항목

- 위 `SF-B2`는 E2E-JV-07에서 grace 초과 뒤 provider 재시작과 신규 outbound 연결 억제를 추적한다.

## 검증 방법

`run_e2e.sh all`은 SF-A1부터 SF-E1까지 실행한다. 단일 ID를 전달하면 해당 시나리오만 실행한다.
`SF-B2`는 남은 단언이 구현되기 전까지 runner의 성공 marker만으로 완료 처리하지 않는다.

## 공통 scenario parity gap — 2026-07-29

다음 공통 scenario에는 Java actual fixture와 runner selector가 없다.

- `SF-B3`, `SF-C3`, `SF-C4`, `SF-C5`
- `SF-F1`, `SF-F2`, `SF-F3`, `SF-F4`, `SF-F5`, `SF-F6`
- `SF-F7`, `SF-F8`, `SF-F9`, `SF-F10`, `SF-F11`
- `SF-G1`, `SF-G2`, `SF-G3`

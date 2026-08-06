# Kotlin RuntimeMonitoring E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-7-monitoring.ko.md`

## 10.0.0 목표 판정

Kotlin E2E는 Java framework의 `ZLinkChannelRuntimeOptions`와 공개 handler·location 타입을 그대로 사용한다. Client는 framework
runtime에 참여하지 않는 JVM HTTP·evidence driver이고, framework operation은 Trigger와
Service role이 수행한다. 기존 socket·location·Spot source marker는 canonical 목표의 부분
증거로만 사용한다.

| 시나리오 | 상태 | 현재 증거 | 남은 gap |
|---|---|---|---|
| MON-A1 | 10.0.0 전환 대상 | `ops-locations` source에 topology·service summary 변경이 기록된다. | Java runtime snapshot 하나의 MeshNode·peer·channel·claim·location·drain field, sequence와 불변성을 Kotlin scenario에서 검증한다. |
| MON-A2 | 10.0.0 전환 대상 | Bare socket source의 연결 marker가 있지만 capability source identity를 사용한 집중 실행은 startup에서 실패한다. | Public peer event·snapshot의 RID, generation, descriptor revision, endpoint, admission, ready, last failure를 재시작 전후로 대조한다. |
| MON-A3 | 10.0.0 전환 대상 | Service weight를 0·100으로 변경하고 Trigger의 admission marker와 location topology 변경을 관측한다. | Channel event, local weight, ready member 수·selectable과 실제 ChannelName request 선택 결과를 각 전이 뒤 비교한다. |
| MON-A4 | 10.0.0 전환 대상 | Service admin 종료 뒤 같은 binary·endpoint로 재시작하고 request·topology down/up을 확인한다. | 정상 replacement와 fresh topology의 `SIGKILL`·lease 만료를 나누고 generation·endpoint·ready member가 최신 snapshot으로 수렴하는지 검증한다. |
| MON-A4A | 구현 | 초기 실행 `logs/20260806-050548-2333121/`은 Service A server-side snapshot을 client readiness로 잘못 사용해 `replacement did not become READY`로 종료했다. Trigger에 public client-side snapshot을 추가한 뒤 `logs/20260806-050934-2482306/`에서 replacement request가 `svc-b`에 의해 처리되는 focused pass를 확인했다. | 없음 |
| MON-A4B | 구현·focused 실행 대상 | Kotlin Service를 공개 `/crash` 경로로 SIGKILL하고 lease 만료 뒤 replacement process를 시작하는 경로를 구현했다. | focused selector에서 stale RID가 선택되지 않고 새 process만 request를 처리하는지 다시 수집한다. |
| MON-A5 | 10.0.0 전환 대상 | Location runtime `STATUS_CHANGED`와 Redis-backed topology 경로가 있다. | Redis 정지·failure grace·복구에서 store event, location state·last success·last failure와 current owner token 재검증을 단언한다. |
| MON-B1 | 11.0.0 전환 대상 | Java runtime과 Kotlin projection에서 publish 전용 snapshot·metric·runtime event와 target count를 제거했다. | 막힌 remote target이 있어도 result-free publish가 시작 뒤 정상 완료하고 publish 전용 관측값이 생기지 않으며 rollback·자동 재시도가 없음을 Kotlin E2E에서 단언한다. |
| MON-B2 | 11.0.0 전환 대상 | Java runtime과 Kotlin projection에서 local target count와 drop event·metric을 제거했다. | 수락 가능한 local target의 단일 처리는 유지하면서 막힌 target의 결과를 public monitoring과 message-flow trace에 기록하지 않음을 Kotlin E2E에서 단언한다. |
| MON-C1 | 10.0.0 전환 대상 | Monitoring handler 예외를 dispatcher가 격리한 뒤 channel messaging이 계속된다. | Application gate, 느린 observer와 정상 observer를 함께 열어 claim progress·request completion·coalescing·sequence gap 후 snapshot resync를 단언한다. |
| MON-D1 | 10.0.0 전환 대상 | 중복 socket source, 비양수 polling interval, 없는 socket·Spot source 구성 실패와 한 번의 service down/up 경로가 있다. | 등록하지 않은 MeshName·0 이하 observer capacity 오류와 비정상 종료·lease 만료·재시작 3회의 sequence·snapshot·event field 제한을 검증한다. |
| MON-A6 | BLOCKED | Kotlin Service가 공개 `spotManager()`·`actorManager()`로 실제 placement 요청을 제출하고 capacity 오류를 확인한다. 최신 focused 실행에서도 `GET /runtime/snapshot`의 `activeSpotCount`가 Spot 생성 전후 모두 `0`으로 관측됐다. 로그 디렉터리: `RuntimeMonitoring/logs/20260806-055113-3466139/`. | 현재 public runtime snapshot이 live Spot 수를 projection하지 않아 CAPACITY_EXCEEDED 이후 destroy/recreate의 공개 증거를 만들 수 없다. marker-only pass는 사용하지 않는다. |
| MON-D1A | 구현·focused 실행 대상 | Kotlin Service가 공개 `routeMeshRuntime().snapshot("missing-mesh")`와 `observe("missing-mesh", ...)`를 호출하고 public validation error를 반환하는 HTTP evidence를 제공한다. | focused selector에서 두 호출이 모두 4xx로 종료하는지 확인한다. |
| MON-D1B | BLOCKED | real Kotlin FilteredService를 RouteMesh peer로 추가하고 SIGKILL 뒤 socket close·replacement를 수행했다. public snapshot에서는 `readyPeerCount=0`까지 수렴했지만 public `/runtime/topology`에 `nodeRid=svc-b-spot`, `state=LOST` row가 20초 이상 남아 same RID replacement가 시작되지 않았다. 최신 focused 로그: `RuntimeMonitoring/logs/20260806-054822-3451483/`. | crash 후 same RID descriptor/lease가 public topology에서 제거·재등록되는 수렴을 확인할 수 없어 세 cycle observer evidence를 완성하지 못했다. marker-only pass는 사용하지 않는다. |

Java runtime의 socket source registry가 capability source 이름을 받아들이는 경계와 canonical scenario
topology를 구현한 뒤 Kotlin runner를 전체 재실행한다.

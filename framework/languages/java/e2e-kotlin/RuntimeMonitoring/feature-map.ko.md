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
| MON-A5 | 10.0.0 전환 대상 | Location runtime `STATUS_CHANGED`와 Redis-backed topology 경로가 있다. | Redis 정지·failure grace·복구에서 store event, location state·last success·last failure와 current owner token 재검증을 단언한다. |
| MON-B1 | 11.0.0 전환 대상 | Java runtime과 Kotlin projection에서 publish 전용 snapshot·metric·runtime event와 target count를 제거했다. | 막힌 remote target이 있어도 result-free publish가 시작 뒤 정상 완료하고 publish 전용 관측값이 생기지 않으며 rollback·자동 재시도가 없음을 Kotlin E2E에서 단언한다. |
| MON-B2 | 11.0.0 전환 대상 | Java runtime과 Kotlin projection에서 local target count와 drop event·metric을 제거했다. | 수락 가능한 local target의 단일 처리는 유지하면서 막힌 target의 결과를 public monitoring과 message-flow trace에 기록하지 않음을 Kotlin E2E에서 단언한다. |
| MON-C1 | 10.0.0 전환 대상 | Monitoring handler 예외를 dispatcher가 격리한 뒤 channel messaging이 계속된다. | Application gate, 느린 observer와 정상 observer를 함께 열어 claim progress·request completion·coalescing·sequence gap 후 snapshot resync를 단언한다. |
| MON-D1 | 10.0.0 전환 대상 | 중복 socket source, 비양수 polling interval, 없는 socket·Spot source 구성 실패와 한 번의 service down/up 경로가 있다. | 등록하지 않은 MeshName·0 이하 observer capacity 오류와 비정상 종료·lease 만료·재시작 3회의 sequence·snapshot·event field 제한을 검증한다. |

Java runtime의 socket source registry가 capability source 이름을 받아들이는 경계와 canonical scenario
topology를 구현한 뒤 Kotlin runner를 전체 재실행한다.

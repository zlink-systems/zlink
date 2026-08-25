# Anchor 치환표 — 이동 커밋 준비물

> `move-plan.ko.md` §3이 요구하는 표다. 계산은 저장소 전체(`.md`·`.json`·`.sh`·`.ts`·`.java`·`.cpp`·`.cs`·`.py`·`.yml`·`.hpp`)를 훑어 만들었다.

> 캠페인 §7에 적힌 "266개 파일·505개 anchor 링크"는 markdown만 센 값이라 실제 규모와 다르다.


## 1. 규모

| 구분 | 참조 수 | 처리 |
|---|---:|---|
| anchor가 그대로 살아 있음 | 2,027 | 경로만 바꾸면 된다 |
| 1:1 문서인데 절 제목이 바뀜 — 자동 매핑됨 | 137 | §2 표대로 치환 |
| 1:1 문서인데 자동 매핑 실패 | 108 | §3에서 손으로 정한다 |
| 1:N으로 갈라진 문서로 가는 참조 | 3,137 | 절 번호를 보고 목적지를 골라야 한다([move-plan §2](move-plan.ko.md)) |

합계 참조 약 7,500건. anchor 없는 경로 참조는 별도다.


## 2. 자동 매핑 — 제목 유사도 0.62 이상

옛 절 제목과 새 절 제목을 번호를 떼고 비교했다. 유사도가 높은 것만 넣었다.

| 옛 문서 | 언어 | 옛 anchor | 새 문서 | 새 anchor | 유사도 | 참조 |
|---|---|---|---|---|---:|---:|
| `11-spot-model` | ko | `#62-유휴-instance-spot-정리` | `03-spot-actor/01-spot-model` | `#6-instance-spot` | 0.81 | 9 |
| `06-framework-api` | ko | `#82-handler-실행-객체와-dependency-수명` | `00-foundation/06-framework-api` | `#11-handler-실행-객체와-dependency-수명` | 1.0 | 8 |
| `06-framework-api` | en | `#82-handler-execution-object-and-dependency-lifetime` | `00-foundation/06-framework-api` | `#11-handler-execution-object-and-dependency-lifetime` | 1.0 | 8 |
| `26-message-flow-tracing` | ko | `#41-실행-중에-기록-수준-변경` | `06-observability/03-message-flow-tracing` | `#5-실행-중-기록-수준-변경과-비용-규칙` | 0.76 | 6 |
| `26-message-flow-tracing` | en | `#41-changing-the-record-level-at-runtime` | `06-observability/03-message-flow-tracing` | `#5-changing-the-record-level-at-runtime-and-the-cost-rule` | 0.8 | 5 |
| `06-framework-api` | ko | `#9-codec` | `00-foundation/06-framework-api` | `#12-codec` | 1.0 | 5 |
| `30-host-relocation-flow` | ko | `#9-대기-중인-message-timer와-session을-옮긴다` | `05-location-relocation/05-host-relocation-flow` | `#12-대기-중인-message-timer와-session을-옮긴다` | 1.0 | 4 |
| `06-framework-api` | en | `#81-handler-filter` | `00-foundation/06-framework-api` | `#10-handler-filter` | 1.0 | 4 |
| `30-host-relocation-flow` | en | `#9-moving-pending-messages-timers-and-sessions` | `05-location-relocation/05-host-relocation-flow` | `#12-moving-pending-messages-timers-and-sessions` | 1.0 | 4 |
| `08-channel-messaging` | ko | `#6-classic-fanout과의-경계` | `02-channel-transport/02-channel-messaging` | `#classic-fanout의-interface와-사용-예` | 0.64 | 3 |
| `10-network-listener-identity` | ko | `#7-시스템-전체-transport-rid와-spot-id-정책` | `02-channel-transport/04-network-listener-identity` | `#6-시스템-전체-transport-rid와-spot-id-정책` | 1.0 | 3 |
| `18-object-routing` | ko | `#25-objectgeneration을-어디에-쓰고-어디에-쓰지-않는가` | `03-spot-actor/08-routing` | `#26-objectgeneration을-어디에-쓰고-어디에-쓰지-않는가` | 1.0 | 3 |
| `18-object-routing` | en | `#25-where-objectgeneration-is-used-and-where-its-not` | `03-spot-actor/08-routing` | `#26-where-objectgeneration-is-used-and-where-its-not` | 1.0 | 3 |
| `06-framework-api` | ko | `#81-handler-filter` | `00-foundation/06-framework-api` | `#10-handler-filter` | 1.0 | 3 |
| `20-session-actor-dispatch` | ko | `#5-actor-relocation-route-barrier` | `04-session/02-session-actor-binding` | `#8-actor-relocation-중-session의-책임` | 0.63 | 3 |
| `06-framework-api` | en | `#9-codec` | `00-foundation/06-framework-api` | `#12-codec` | 1.0 | 3 |
| `30-host-relocation-flow` | ko | `#11-shutdown과-relocate의-경쟁` | `05-location-relocation/05-host-relocation-flow` | `#14-shutdown과-relocate의-경쟁` | 1.0 | 3 |
| `21-location-runtime` | en | `#8-when-a-store-response-isnt-received` | `05-location-relocation/01-location-runtime` | `#10-when-a-store-response-isnt-received` | 1.0 | 2 |
| `07-channel-topology` | ko | `#51-automatic은-rid가-더-작은-meshnode만-연결을-시작한다` | `02-channel-transport/01-channel-topology` | `#automatic은-rid가-더-작은-meshnode만-연결을-시작한다` | 1.0 | 2 |
| `21-location-runtime` | en | `#24-how-different-languages-read-and-write-the-same-redis-record` | `05-location-relocation/01-location-runtime` | `#34-how-different-languages-read-and-write-the-same-redis-record` | 1.0 | 2 |
| `21-location-runtime` | ko | `#8-store-응답을-받지-못했을-때` | `05-location-relocation/01-location-runtime` | `#10-store-응답을-받지-못했을-때` | 1.0 | 2 |
| `04-message-model` | ko | `#23-framework-json-v1-typed-payload-profile` | `00-foundation/05-message-model` | `#5-framework-json-v1-typed-payload-profile` | 0.97 | 2 |
| `30-host-relocation-flow` | en | `#8-the-order-for-relocating-one-unit` | `05-location-relocation/05-host-relocation-flow` | `#9-the-order-for-relocating-one-unit-owned-by-04` | 0.82 | 2 |
| `30-host-relocation-flow` | en | `#11-the-race-between-shutdown-and-relocate` | `05-location-relocation/05-host-relocation-flow` | `#14-the-race-between-shutdown-and-relocate` | 1.0 | 2 |
| `21-location-runtime` | ko | `#63-이전-owner로-도착한-message를-새-owner에게-전달한다` | `05-location-relocation/01-location-runtime` | `#73-이전-owner로-도착한-message를-새-owner에게-전달한다` | 1.0 | 2 |
| `21-location-runtime` | ko | `#64-운영-도구에서-현재-위치를-조회한다` | `05-location-relocation/01-location-runtime` | `#74-운영-도구에서-현재-위치를-조회한다` | 1.0 | 2 |
| `30-host-relocation-flow` | ko | `#8-unit-하나를-이전하는-순서` | `05-location-relocation/05-host-relocation-flow` | `#9-unit-하나를-이전하는-순서-04를-따른다` | 0.76 | 2 |
| `21-location-runtime` | en | `#63-delivering-a-message-arriving-at-a-previous-owner-to-the-new-owner` | `05-location-relocation/01-location-runtime` | `#73-delivering-a-message-arriving-at-a-previous-owner-to-the-new-owner` | 1.0 | 2 |
| `30-host-relocation-flow` | ko | `#10-relocate-완료와-실패` | `05-location-relocation/05-host-relocation-flow` | `#13-relocate-완료와-실패` | 1.0 | 2 |
| `08-channel-messaging` | ko | `#9-검증-요구` | `02-channel-transport/02-channel-messaging` | `#10-검증-요구` | 1.0 | 2 |
| `06-framework-api` | ko | `#13-오류-kind` | `00-foundation/06-framework-api` | `#19-오류-kind` | 1.0 | 1 |
| `21-location-runtime` | ko | `#61-message를-받은-node에서-instance-spot을-처음-만든다` | `05-location-relocation/01-location-runtime` | `#71-message를-받은-node에서-instance-spot을-처음-만든다` | 1.0 | 1 |
| `15-spot-actor` | ko | `#7-실패-처리-범위` | `03-spot-actor/05-spot-actor-membership` | `#8-실패-처리-범위` | 1.0 | 1 |
| `06-framework-api` | en | `#13-error-kinds` | `00-foundation/06-framework-api` | `#19-error-kinds` | 1.0 | 1 |
| `21-location-runtime` | en | `#61-first-creating-an-instance-spot-on-the-node-that-received-the-message` | `05-location-relocation/01-location-runtime` | `#71-first-creating-an-instance-spot-on-the-node-that-received-the-message` | 1.0 | 1 |
| `15-spot-actor` | en | `#7-failure-handling-scope` | `03-spot-actor/05-spot-actor-membership` | `#8-failure-handling-scope` | 1.0 | 1 |
| `24-runtime-monitoring` | en | `#coalescing` | `06-observability/01-runtime-monitoring` | `#72-coalescing` | 1.0 | 1 |
| `28-relocation-flow` | en | `#46-open-the-target-queue-progressively-with-existing-work-first` | `05-location-relocation/04-relocation-flow` | `#46-target-opens-the-queue-progressively-starting-with-existing-work` | 0.72 | 1 |
| `30-host-relocation-flow` | en | `#7-relocation-units-and-execution-order` | `05-location-relocation/05-host-relocation-flow` | `#7-relocation-units-and-batch-order` | 0.82 | 1 |
| `30-host-relocation-flow` | en | `#14-contract-test-verification-requirements` | `05-location-relocation/05-host-relocation-flow` | `#17-implementation-and-contract-test-verification-requirements` | 0.78 | 1 |
| `24-runtime-monitoring` | ko | `#합치기` | `06-observability/01-runtime-monitoring` | `#72-합치기` | 1.0 | 1 |
| `30-host-relocation-flow` | en | `#10-relocate-completion-and-failure` | `05-location-relocation/05-host-relocation-flow` | `#13-relocate-completion-and-failure` | 1.0 | 1 |
| `18-object-routing` | ko | `#24-이전-owner-route에-도착한-message` | `03-spot-actor/08-routing` | `#25-이전-owner-route에-도착한-message` | 1.0 | 1 |
| `09-client-server-channel` | ko | `#5-weight와-target-선택` | `02-channel-transport/03-client-server-channel` | `#4-weight와-target-선택` | 1.0 | 1 |
| `30-host-relocation-flow` | ko | `#12-state별-admission` | `05-location-relocation/05-host-relocation-flow` | `#15-state별-admission` | 1.0 | 1 |
| `30-host-relocation-flow` | ko | `#7-relocation-unit과-실행-순서` | `05-location-relocation/05-host-relocation-flow` | `#7-relocation-unit과-batch-순서` | 0.85 | 1 |
| `30-host-relocation-flow` | ko | `#14-contract-test-검증-요구` | `05-location-relocation/05-host-relocation-flow` | `#17-구현-및-contract-test-검증-요구` | 0.84 | 1 |
| `04-message-model` | en | `#23-the-framework-json-v1-typed-payload-profile` | `00-foundation/05-message-model` | `#5-the-framework-json-v1-typed-payload-profile` | 0.98 | 1 |
| `08-channel-messaging` | en | `#6-the-boundary-with-classic-fanout` | `02-channel-transport/02-channel-messaging` | `#7-the-boundary-with-classic-fanout-reserved-liveness-beacon-topic` | 0.66 | 1 |
| `16-spot-address-messaging` | ko | `#4-direct-message로-instance-spot-생성을-허용하는-방법` | `03-spot-actor/06-spot-address-messaging` | `#4-cold-activation-message로-instance-spot을-처음-만드는-방법` | 0.67 | 1 |
| `40-internal-layering` | en | `#5-registration-declaration-is-validated-only-once-at-start` | `00-foundation/08-layering` | `#5-registration-declarations-are-validated-only-once-at-startup` | 0.93 | 1 |
| `25-runtime-metrics` | en | `#5-host-relocation-and-shutdown` | `06-observability/02-runtime-metrics` | `#8-host-relocation-and-shutdown` | 1.0 | 1 |
| `18-object-routing` | en | `#24-a-message-arriving-at-a-previous-owner-route` | `03-spot-actor/08-routing` | `#25-a-message-arriving-at-a-previous-owner-route` | 1.0 | 1 |
| `09-client-server-channel` | en | `#5-weight-and-target-selection` | `02-channel-transport/03-client-server-channel` | `#4-weight-and-target-selection` | 1.0 | 1 |
| `10-network-listener-identity` | en | `#7-system-wide-transport-rid-and-spot-id-policy` | `02-channel-transport/04-network-listener-identity` | `#6-system-wide-transport-rid-and-spot-id-policy` | 1.0 | 1 |
| `07-channel-topology` | en | `#51-automatic-only-the-meshnode-with-the-smaller-rid-starts-the-connection` | `02-channel-transport/01-channel-topology` | `#automatic-only-the-meshnode-with-the-smaller-rid-starts-the-connection` | 1.0 | 1 |
| `25-runtime-metrics` | ko | `#5-host-relocation과-shutdown` | `06-observability/02-runtime-metrics` | `#8-host-relocation과-shutdown` | 1.0 | 1 |
| `21-location-runtime` | en | `#64-querying-the-current-location-from-operational-tools` | `05-location-relocation/01-location-runtime` | `#74-querying-the-current-location-from-operational-tools` | 1.0 | 1 |
| `30-host-relocation-flow` | en | `#12-admission-per-state` | `05-location-relocation/05-host-relocation-flow` | `#15-admission-per-state` | 1.0 | 1 |
| `21-location-runtime` | ko | `#24-여러-언어가-같은-redis-record를-읽고-쓰는-방법` | `05-location-relocation/01-location-runtime` | `#34-여러-언어가-같은-redis-record를-읽고-쓰는-방법` | 1.0 | 1 |
| `08-channel-messaging` | en | `#9-verification-requirements` | `02-channel-transport/02-channel-messaging` | `#10-verification-requirements` | 1.0 | 1 |
| `26-message-flow-tracing` | en | `#4-how-the-application-sets-the-recording-scope` | `06-observability/03-message-flow-tracing` | `#4-how-the-application-sets-the-recording-scope-level-and-sampling` | 0.81 | 1 |
| `07-channel-topology` | ko | `#8-routemesh-ss-message-크기와-mailbox-상한` | `02-channel-transport/01-channel-topology` | `#11-routemesh-ss-message-크기와-mailbox-상한` | 1.0 | 1 |
| `07-channel-topology` | ko | `#52-weight-변경은-연결을-다시-만들지-않는다` | `02-channel-transport/01-channel-topology` | `#weight-변경은-연결을-다시-만들지-않는다` | 1.0 | 1 |

## 3. 손으로 정해야 하는 것

자동 매핑이 실패한 항목이다. 대부분 절이 다른 문서로 옮겨 갔거나 여러 절로 갈라진 경우다.

| 옛 문서 | 언어 | 옛 anchor | 참조 | 실패 사유 |
|---|---|---|---:|---|
| `20-session-actor-dispatch` | ko | `#4-session이-actor-route를-보관하는-방법` | 6 | 최고 유사도 0.53 — 1. Session–Actor binding 개요 |
| `24-runtime-monitoring` | ko | `#3-현재-상태-조회와-변화-관찰` | 6 | 최고 유사도 0.45 — 1. Runtime 상태 조회 개요 |
| `20-session-actor-dispatch` | en | `#4-how-a-session-holds-an-actor-route` | 6 | 최고 유사도 0.49 — 8. The Session's Responsibility During Actor Relocation |
| `24-runtime-monitoring` | en | `#3-querying-current-state-and-observing-changes` | 6 | 최고 유사도 0.49 — 8. Querying An Object's Current Location |
| `22-location-store-redis` | en | `#7-registration-lifetime-and-the-official-redis-provider` | 4 | 최고 유사도 0.47 — 8. Official Redis Provider — Counter Issuance |
| `22-location-store-redis` | ko | `#7-등록-수명과-공식-redis-provider` | 4 | 최고 유사도 0.62 — 8. 공식 Redis provider — Counter 발급 |
| `21-location-runtime` | en | `#13-registration-conditions-and-lifetime` | 3 | 최고 유사도 0.45 — 12. Implementation And Contract-Test Verification Requirements |
| `17-stage-wrapper-on-spot` | ko | `#5-timer` | 3 | 최고 유사도 0.59 — 5. Timer와 Yield |
| `20-session-actor-dispatch` | en | `#5-actor-relocation-route-barrier` | 3 | 최고 유사도 0.42 — 8.1 Seal, Held Messages, And Route Switchover |
| `08-channel-messaging` | ko | `#선택-순서` | 3 | 최고 유사도 0.53 — 가중 라운드로빈 선택 순서 |
| `08-channel-messaging` | ko | `#32-channelname-select-one` | 3 | 최고 유사도 0.62 — 1. Node direct와 ChannelName select-one 개요, 공통 API 예시 |
| `25-runtime-metrics` | ko | `#4-object와-stream` | 3 | 최고 유사도 0.35 — 6. Object 수·capacity와 relocation 계기 |
| `25-runtime-metrics` | en | `#4-object-and-stream` | 3 | 최고 유사도 0.41 — 9. Location And Telemetry |
| `17-stage-wrapper-on-spot` | en | `#5-timer` | 3 | 최고 유사도 0.50 — 5. Timer And Yield |
| `21-location-runtime` | ko | `#13-등록-조건과-수명` | 3 | 최고 유사도 0.18 — 9. Restore·완료 기록과 Store의 관계 |
| `20-session-actor-dispatch` | ko | `#3-inbound-dispatch와-reply` | 2 | 최고 유사도 0.47 — 5. Bind와 relay |
| `45-internal-routing-and-cache` | ko | `#2-이동과-캐시가-만나는-지점--성능-절벽` | 2 | 옛 문서에 그 anchor 없음 |
| `08-channel-messaging` | en | `#selection-order` | 2 | 최고 유사도 0.59 — Weighted Round-Robin Selection Order |
| `20-session-actor-dispatch` | ko | `#51-session-actor-위치-갱신-message` | 2 | 옛 문서에 그 anchor 없음 |
| `44-internal-relocation-continuity` | ko | `#1-네-개의-경계` | 2 | 최고 유사도 0.40 — 2. 각 주체의 책임 |
| `20-session-actor-dispatch` | en | `#3-inbound-dispatch-and-reply` | 2 | 최고 유사도 0.55 — 5. Bind And Relay |
| `08-channel-messaging` | en | `#32-channelname-select-one` | 2 | 최고 유사도 0.49 — 1. Node Direct And ChannelName Select-One Overview, Common API Example |
| `06-framework-api` | ko | `#16-missing-object-생성--cold-activation-순서` | 2 | 옛 문서에 그 anchor 없음 |
| `01-glossary` | ko | `#anchor` | 2 | 옛 문서에 그 anchor 없음 |
| `21-location-runtime` | en | `#1-scope-and-responsibility` | 1 | 최고 유사도 0.53 — 2. Roles And Responsibilities — Provider vs. Framework |
| `20-session-actor-dispatch` | ko | `#6-failure-처리` | 1 | 최고 유사도 0.30 — 3. Startup 조건 |
| `10-network-listener-identity` | en | `#7-system-wide-routing-id-policy` | 1 | 옛 문서에 그 anchor 없음 |
| `20-session-actor-dispatch` | en | `#51-session-actor-location-update-message` | 1 | 옛 문서에 그 anchor 없음 |
| `20-session-actor-dispatch` | en | `#6-failure-handling` | 1 | 최고 유사도 0.59 — 12. Failure And Errors |
| `24-runtime-monitoring` | en | `#22-topology-state` | 1 | 최고 유사도 0.39 — 5. Topology State — RouteMesh, ClientServer, Automatic Fanout |
| `24-runtime-monitoring` | en | `#21-host-state` | 1 | 최고 유사도 0.48 — 3. Host State — Values Read At Once |
| `19-stream-session` | ko | `#7-등록-모델` | 1 | 최고 유사도 0.32 — 3. 등록과 startup 검증 |
| `24-runtime-monitoring` | ko | `#22-topology-상태` | 1 | 최고 유사도 0.34 — 5. Topology 상태 — RouteMesh·ClientServer·automatic fanout |
| `24-runtime-monitoring` | ko | `#21-host-상태` | 1 | 최고 유사도 0.54 — 3. Host 상태 — 한 번에 읽는 값 |
| `06-framework-api` | ko | `#12-spot-actor와-stream-owner` | 1 | 최고 유사도 0.49 — 21. Dispatch 실패 action owner |
| `44-internal-relocation-continuity` | en | `#1-four-boundaries` | 1 | 최고 유사도 0.41 — 12. Guarantees And Non-Guarantees |
| `19-stream-session` | en | `#7-registration-model` | 1 | 최고 유사도 0.53 — 3. Registration And Startup Validation |
| `45-internal-routing-and-cache` | en | `#2-where-a-move-meets-the-cache--a-performance-cliff` | 1 | 옛 문서에 그 anchor 없음 |
| `16-spot-address-messaging` | en | `#4-how-to-allow-instance-spot-creation-via-a-direct-message` | 1 | 최고 유사도 0.60 — 4. Cold Activation — How To Create An Instance Spot For The First Time Via A Message |
| `30-host-relocation-flow` | en | `#82-the-common-order-every-actor-and-spot-follows` | 1 | 최고 유사도 0.41 — 10.2 PerActor User Spot |
| `30-host-relocation-flow` | ko | `#82-모든-actor와-spot이-따르는-공통-순서` | 1 | 최고 유사도 0.47 — 10.2 PerActor User Spot |
| `06-framework-api` | en | `#12-spot-actor-and-stream-owner` | 1 | 최고 유사도 0.48 — 21. Dispatch Failure Action Owner |
| `21-location-runtime` | ko | `#1-범위와-책임` | 1 | 최고 유사도 0.27 — 6.1 Read와 CAS |
| `06-framework-api` | en | `#16-creating-a-missing-object--cold-activation-sequence` | 1 | 옛 문서에 그 anchor 없음 |
| `19-stream-session` | ko | `#5-codec-계층-분리` | 1 | 최고 유사도 0.39 — 6. Payload 변환과 codec 경계 |
| `19-stream-session` | en | `#5-codec-layer-separation` | 1 | 최고 유사도 0.41 — 5. Reply Correlation |
| `26-message-flow-tracing` | ko | `#4-application은-기록-범위를-어떻게-정하는가` | 1 | 최고 유사도 0.26 — 4. 기록 범위 설정 — level과 sampling |
| `20-session-actor-dispatch` | ko | `#51-…` | 1 | 옛 문서에 그 anchor 없음 |
| `06-framework-api` | ko | `#12-...` | 1 | 옛 문서에 그 anchor 없음 |
| `11-spot-model` | ko | `#42-...` | 1 | 옛 문서에 그 anchor 없음 |
| `30-host-relocation-flow` | ko | `#9-...` | 1 | 옛 문서에 그 anchor 없음 |
| `31-failure-failover-policy` | ko | `#44-...` | 1 | 옛 문서에 그 anchor 없음 |
| `01-glossary` | ko | `#<anchor>` | 1 | 옛 문서에 그 anchor 없음 |
| `51-internal-service-wire-protocol` | ko | `#9-…` | 1 | 옛 문서에 그 anchor 없음 |
| `01-glossary` | ko | `#*` | 1 | 옛 문서에 그 anchor 없음 |

## 4. 재계산 방법

스크립트는 `doc/plan/spec-server-reorg/build-anchor-map.py`다. 문서를 더 고치면 다시 돌려 이 표를 갱신한다.


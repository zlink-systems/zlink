# 재구성 스펙 ↔ cpp 구현 대조 (2차, 6개 주제)

검토 기준: `22949bcedb67c01e9f4c70d7fd0cd194e817c676`
검토 범위: `framework/doc/framework/common/spec/server/{00-foundation,01-execution,02-channel-transport,
03-spot-actor,05-location-relocation,06-observability}/*.ko.md`(검증 요구 섹션 전수 열람,
30개 문서),`framework/languages/cpp/framework/{src,include}`(주로 `contracts/errors`,
`contracts/configuration/framework_options.hpp`, `contracts/timers/timer.hpp`,
`contracts/actors/actor.hpp`, `contracts/locations/options.hpp`,
`runtime/dispatch/{application_job_queue,receive_batch_budget,dispatch_limits}.hpp`,
`runtime/mesh/{service_liveness_registry,route_mesh_runtime_options_service,
service_topology_registry}`, `runtime/channels/channel_runtime.cpp`,
`runtime/locations/{location_repository,in_memory_location_store,in_memory_store_providers,
store_location_resolvers}.hpp`, `runtime/execution/serial_execution_queue.cpp`,
`runtime/client_server/raw_client_server_owner.cpp`),
`framework/runtime/protocol/generated/cpp/service_wire_constants.hpp`,
`framework/languages/cpp/tests/Zlink.Framework.ContractTests/test_cpp_framework_layout_contract.cpp`,
`scripts/verify-framework-submit-api.sh`, `scripts/verify-framework-instance-spot-contracts.sh`

도달하지 못한 범위: 이 라운드는 지정된 우선순위(검증 요구 → 수치/닫힌집합 → 부정규칙 →
G9–G21)를 따라 **표본 대조**만 수행했다 — 30개 검증 요구 섹션 안의 개별 불릿(총
250개 이상)을 전수 대조하지 않았다. 특히: 01-execution의 handler-turn/cancellation/
payload-ownership 세부 항목(재진입 금지, deserialize 시점, thread-local 미사용 등
"내부 확인 조건"으로 분류된 항목), 02-channel-transport의 automatic RouteMesh 연결
경합·duplicate-pipe admission 세부 절차, 03-spot-actor의 08-routing 전체(cache
invalidation·Message Follow 세부 규칙), 05-location-relocation의 04·05 문서의
chunk/checksum/재전송 절차 대부분, 06-observability의 message-flow-tracing·
flow-correlation의 개별 event_id/phase/attribute 닫힌 집합 전수 비교는 검토하지
못했다. G9–G17(session 주제 소유)은 이미 `topics/04-session/gap-cpp.md`에서 cpp
근거가 확정돼 있어 재확인하지 않았다.

## 주제별 대조

### 00-foundation

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| ErrorKind 13개·번호 | 일치 | `framework/languages/cpp/framework/include/zlink/framework/contracts/errors/error.hpp:13-27`(0~12, 13개) | `framework/doc/.../00-foundation/07-framework-error-model.ko.md:28-40`의 이름·번호와 1:1 대응(NotFound=0…InternalFailure=12). |
| 재시도 hint 부재 | 일치 | `error.hpp:90-120`(`framework_exception_t`는 kind·message·std::error_code만 노출, retry hint 필드 없음) | |
| 30초 host shutdown 기본 deadline | 일치 | `framework/languages/cpp/framework/include/zlink/framework/contracts/configuration/app.hpp:92`(`shutdown (... deadline = std::chrono::seconds (30) ...)`) | 08-layering §7 "종료" 검증 요구 대응. |
| Typed Rejected와 ErrorKind.Rejected exception 구분 | 판단 불가 | — | 표본 검색 범위에서 typed Rejected 반환 경로를 특정하지 못함. |

### 01-execution

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| One-way call이 admission 수락 시 "반환 데이터 없이 완료" | 일치(문서 존재 확인) | `framework/doc/.../01-execution/01-submit-and-completion.ko.md:123-124` | cpp 쪽 구현 관찰(코드가 실제로 그렇게 완료하는지)은 이번 라운드에서 별도 확인 못함 — 문서 표현만 확인. |
| 동기 `TrySubmit` 계열 미제공 | 일치 | `scripts/verify-framework-submit-api.sh`(cpp public source에 `TrySubmit`/`try_submit` 없음을 검사하는 gate가 현재 통과) 및 `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/task.hpp`에 동기 terminator 없음(육안 확인) | 정적 gate 실행 결과는 별도 재현하지 않고 스크립트 로직만 확인. |
| Actor Join: handler당 최대 64개 | 일치 | `framework/languages/cpp/framework/src/runtime/execution/serial_execution_queue.cpp:270-273`("A Framework handler may defer at most 64 Actor joins") | |
| Actor Join 기본 timeout 5초 | 일치 | `framework/languages/cpp/framework/include/zlink/framework/contracts/actors/actor.hpp:627`(`_timeout{5000}`) | |
| Actor Join: request당 1 MiB, 합계 8 MiB | 판단 불가 | — | `actor_authority_maximum_bytes = 1024*1024`(`framework/languages/cpp/framework/src/runtime/locations/actor_authority_payload.hpp:85`)는 존재하나 Join 요청 자체의 1 MiB/8 MiB 한도와 동일 개념인지 대응시키지 못함; 별도 상수를 찾지 못함. |
| Timer overrun policy 3값(SkipLateTicks/CatchUpBounded/DelayNextTick) | 일치 | `framework/languages/cpp/framework/include/zlink/framework/contracts/timers/timer.hpp:19-23`(`skip_late_ticks=0, catch_up_bounded=1, delay_next_tick=2`) | 기본값도 `skip_late_ticks`(`timer.hpp:28`)로 스펙과 일치. |
| Pressure 80% pause / 60% resume | 일치 | `framework/languages/cpp/framework/src/runtime/dispatch/application_job_queue.hpp:32-33`(`pause_threshold_percent=80, resume_threshold_percent=60`) | 커스텀 설정 시 `pause>resume`, `resume<=99` 등 부등식 검사도 존재(`:644-648`). |
| Core profile·Application job queue profile 기본값 `Balanced` | 판단 불가 | — | enum 이름은 확인했으나(`framework_options.hpp` 부근) 이번 라운드에서 기본값 리터럴까지 좁혀 확인하지 못함. |

### 02-channel-transport

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| Weight 0/기본 100/상한 10000, -1과 10001 거부 (ClientServer) | 일치 | `framework/languages/cpp/framework/include/zlink/framework/contracts/configuration/framework_options.hpp:585-587` | |
| Weight 0/100/10000 (RouteMesh Channel) | 일치 | `framework/languages/cpp/framework/src/runtime/mesh/route_mesh_runtime_options_service.cpp:45-47`("channel weight must be in range 0..10000") | |
| Weight 0/100/10000 (Placement) | 일치 | `route_mesh_runtime_options_service.cpp:87-89`("placement weight must be in range 0..10000") | |
| Weight 0/100/10000 (service/topology 등록) | 일치 | `framework/languages/cpp/framework/src/runtime/mesh/service_topology_registry.cpp:36-38,62`; `framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:799-801,923-925` | 5개 지점 모두 동일한 `0..10000` 경계로 구현됨(런타임 변경 setter 포함). |
| RouteMesh·ClientServer probe 5초 간격, half-open 15초 | 일치 | `framework/languages/cpp/framework/src/runtime/mesh/service_liveness_registry.hpp:34-35`(`probe_interval = seconds(5), peer_timeout = seconds(15)`) | `raw_mesh_node_owner.cpp`(RouteMesh)가 이 registry 타입을 직접 사용(`raw_mesh_node_owner.hpp:184,521`)해 5s/15s를 공유. ClientServer 쪽이 같은 타입을 쓰는지는 `raw_client_server_owner.cpp:44-45`(admission/probe-request timeout 5000ms — probe 자체와는 별개 값)만 확인, 동일 registry 재사용 여부는 판단 보류. |
| ClientServer admission·probe-request timeout 5000ms | 일치 | `framework/languages/cpp/framework/src/runtime/client_server/raw_client_server_owner.cpp:44-45` | |
| 수신 상한 3축(건수·byte·경과 시간) 정확한 값 — 스펙은 "판정하지 않는다"고 명시 | G18 참고(해당 섹션) | `framework/languages/cpp/framework/src/runtime/dispatch/dispatch_limits.hpp:15-17` | cpp가 고른 구체 값: 64 messages / 1 MiB / 2ms. |

### 03-spot-actor

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| Wire command 20·24·36·38·42·43·44·47·48·49·50·51 존재, 45 결번(44→46 점프) | 일치 | `framework/runtime/protocol/generated/cpp/service_wire_constants.hpp:24-53`(`reply=20, actorSend=24, boundSessionSend=36, boundSessionBind=38, sessionRelocationSeal=42, sessionRelocationSealed=43, sessionRelocationRoute=44, replyRelayAck=46, userSpotCreate=47, userSpotClose=48, actorCreate=49, messageFollow=50, boundSessionReplaced=51`) | 03-spot-actor/06-spot-address-messaging.ko.md §10이 요구하는 command 47·20·48 세트도 이 헤더에 존재. |
| Actor 생성 terminal 값 집합(창조 결과) — G19 참고 | 일치(cpp는 `failed` 사용) | `framework/languages/cpp/framework/src/runtime/locations/location_repository.hpp:249-254`(`creation_terminal_state_t{created=1, rejected=2, failed=3}`) | `Aborted`라는 별도 leaf 이름은 cpp에 없음 — cpp는 스펙 14 §6.4 쪽("Failed") 표현을 따름. |

### 05-location-relocation

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| `2^63-1` 다음 세대 발급 → GenerationExhausted | 일치 | `framework/languages/cpp/framework/src/runtime/locations/in_memory_location_store.hpp:437,582,608,651,705,911,1016,1119`(`owner_lease_generation_exhausted_t`/`authority_generation_exhausted_t`), `:1603`(`std::numeric_limits<std::int64_t>::max ()`로 경계 계산) | |
| 목록 조회 cursor 최대 4096 bytes | 일치 | `framework/languages/cpp/framework/src/runtime/locations/location_repository.hpp:124`("authority scan cursor must contain 1..4096 bytes") | |
| 페이지 최대 1,000개 | 일치 | `framework/languages/cpp/framework/src/runtime/locations/{in_memory_store_providers.hpp:114, store_location_resolvers.hpp:707-709, in_memory_location_store.hpp:256,379,731, provider_location_repository.hpp:446,2532}` | 7개 지점 모두 `1000` 동일. |
| 페이지 최대 4 MiB | 일치 | `framework/languages/cpp/framework/src/runtime/locations/in_memory_store_providers.hpp:291`("location write exceeds 4 MiB"); `store_location_resolvers.hpp:604`("Location Store encoded object page exceeds 4 MiB") | |
| 같은 요청 결과 재조회 TTL 5분 | 일치 | `framework/languages/cpp/framework/src/runtime/locations/in_memory_location_store.hpp:838`, `provider_location_repository.hpp:684`(`operation_deadline + std::chrono::minutes (5)`) | |
| relocation blob 상한 64 MiB | 일치(부가 확인) | `framework/languages/cpp/framework/src/runtime/locations/in_memory_store_providers.hpp:357`("relocation blob exceeds 64 MiB"); `framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:5472,5516,5552`("... exceeds the 64 MiB limit") | R60 계열(session gap)의 64 MiB와 동일 값 재확인. |
| `MessageFollowDuration` 기본 30초 | 일치 | `framework/languages/cpp/framework/include/zlink/framework/contracts/locations/options.hpp:27`(`message_follow_duration{30000}`) | |
| Host relocation cutover 대기 기본 1000ms | 일치 | `options.hpp:34`(`relocation_cutover_wait_timeout{1000}`) | session gap R60과 동일. |
| Host relocation `Shutdown` 기본 deadline 30초 | 일치 | `framework/languages/cpp/framework/include/zlink/framework/contracts/configuration/app.hpp:92` | 05-host-relocation-flow.ko.md 표의 "Shutdown"과 대응. |
| Interruption 목표 source-local 1초 실측 | 판단 불가 | — | 값 자체(위 cutover 1000ms)는 확인했으나, elapsed 실측 assert 테스트는 이번 라운드에서 찾지 못함(session gap과 동일 결론). |
| 페이지 항목 상한 표현 불일치(1,024 아님/2,048-key 아님) — G20 참고 | 불일치(문서 내부 모순, cpp 값은 확인됨) | 위 "페이지 최대 1,000개" 행 | cpp 실제 값은 1,000이며 1,024도 2,048도 아니다 — 두 표현 모두 cpp 관찰과 불일치. |

### 06-observability

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| Automatic fanout 15초 record timeout | 일치(값 근거는 02-channel-transport의 `peer_timeout=15초`와 동일 registry 추정) | `framework/languages/cpp/framework/src/runtime/mesh/service_liveness_registry.hpp:35` | 이 15초 값이 fanout publisher record timeout과 동일 상수인지, `runtime_monitoring.ko.md`가 요구하는 public status 노출까지는 이번 라운드에서 별도 추적하지 못함(판단 보류 요소 포함). |
| Public status에 endpoint·descriptor revision·owner lease·claim·reservation·native handle·raw event DTO 없음 | 판단 불가 | — | 공개 monitoring 표면 전체를 훑어 이 7개 항목의 부재를 개별 확인하지 못함. |
| Metric label에 topic·Actor ID·Spot ID·RID·endpoint·correlation ID·flow ID 없음 | 판단 불가 | — | 표본 범위 밖. |

## G 항목 — 이 구현의 실제 동작

| G# | 이 구현이 하는 것 | 근거 (파일:줄) |
|---|---|---|
| G18 | 수신 독점 상한 3축의 cpp 구체값: 최대 64 messages, 최대 1 MiB(`1u*1024u*1024u`), 최대 경과 2ms. 세 조건 중 하나라도 먼저 닿으면 `exhausted()`가 true가 되어 회전이 끊기고, `process_core_frames`가 다음 peer로 넘어간다(`can_receive`가 `messages==0`일 때는 byte/시간 조건을 건너뛰어 최소 1개는 항상 받는다). | `framework/languages/cpp/framework/src/runtime/dispatch/dispatch_limits.hpp:15-17`(`receive_batch_messages=64, receive_batch_bytes=1u*1024u*1024u, receive_batch_time{2}`); `framework/languages/cpp/framework/src/runtime/dispatch/receive_batch_budget.hpp:13-46`; 호출부 `framework/languages/cpp/framework/src/runtime/streams/stream_host_service.cpp:1698-1704` |
| G19 | Actor/Spot 생성 terminal 상태 enum은 `created / rejected / failed` 3값이며, 스펙 14 §6.4의 "Failed" 쪽 이름과 일치한다. cpp 코드베이스 전체에서 이 결과 enum 옆에 별도 "Aborted" leaf를 두는 타입을 찾지 못했다(15 §2가 말하는 "Abort"는 §4 relocation Restore 실패("relay-ready reply accepted 전 명시적 실패로 폐기") 계열의 다른 개념으로 별도 코드 경로를 갖고, 이 creation-terminal enum과는 이름도 코드 경로도 분리돼 있다). | `framework/languages/cpp/framework/src/runtime/locations/location_repository.hpp:249-254` |
| G20 | Location 목록 조회 페이지의 실제 상한은 1,000개(byte 상한은 별도로 4 MiB) — "1,024가 아니다"(21 §6.4)와 "2,048-key가 아니다"(22)라는 두 부정 표현 모두, cpp가 실제로 사용하는 1,000이라는 값과는 어느 쪽도 일치하지 않는 서로 다른 숫자를 부정하고 있다. | `framework/languages/cpp/framework/src/runtime/locations/store_location_resolvers.hpp:707-709` 외 6곳(위 05-location-relocation 표 참고) |
| G21 | cpp 공개 contracts 헤더 전체에서 "connection projection API"(문서 45 §4가 전제하는, framework의 RID별 승인·weight·active 상태를 Core 하위 계층에 투영해 socket을 합치는 API)에 대응하는 타입이나 함수를 찾지 못했다 — `fanout_publisher_connection_snapshot_t`(모니터링 전용 snapshot)만 존재하며 이는 승인 정보를 Core에 밀어넣는 API가 아니다. 문서 45 자신도 §4에서 "이 경로가 없으면 per-server connection과 framework selector를 유지한다"고 명시해 이 API가 현재 존재하지 않는 전제를 스스로 인정한다. | `framework/languages/cpp/framework/include/zlink/framework/contracts/monitoring/fanout_runtime.hpp:32-53,68`(유일하게 발견된 근접 타입); `framework/doc/framework/common/spec/server/03-spot-actor/08-routing.ko.md:227-230`(자기 인정 문구) |

## TEST — needle 문장 확인

| 파일 | needle 수 | 새 문서에 존재 | 깨지는 것 |
|---|---|---|---|
| `test_cpp_framework_layout_contract.cpp` → `14-actor-model.ko.md` → `03-spot-actor/04-actor-model.ko.md` | 12 | 12/12 (글자 단위 확인) | 없음 |
| `test_cpp_framework_layout_contract.cpp` → `06-framework-api.ko.md` → `00-foundation/06-framework-api.ko.md` | 4 | 4/4 (글자 단위 확인) | 없음 |
| `test_cpp_framework_layout_contract.cpp` → `20-session-actor-dispatch.ko.md` → `04-session/02-session-actor-binding.ko.md` | 3 | 3/3 (글자 단위 확인, session 주제이나 이 테스트 케이스에 한해 확인) | 없음(단, 테스트 자체가 옛 경로 `20-session-actor-dispatch.ko.md`를 계속 여는 문제는 1차 라운드 gap-cpp.md에 이미 기록됨) |
| `scripts/verify-framework-submit-api.sh` → `05-async-execution-policy.ko.md`/`06-framework-api.ko.md`/`12-spot-messaging.ko.md`(옛 경로, 스크립트가 실제로 여는 파일) | 각 3~4개(공백 정규화 없이 원문 그대로 검사) | 대응하는 새 문서(`01-execution/01-submit-and-completion.ko.md`, `00-foundation/06-framework-api.ko.md`, `03-spot-actor/02-spot-messaging.ko.md`)에서 표본 조각(`반환 데이터 없이 완료`, `Publish 완료는 handler 실행 결과가 아니라 local outbound admission`, `monitoring snapshot, metric 또는 runtime event로 제공하지 않는다`) 존재 확인 | 스크립트가 여전히 옛 경로 파일을 열어 검사하므로 지금은 안 깨짐; 옛 문서가 삭제되면 즉시 깨짐(경로 갱신 필요) — 1차 세션 TEST 결론과 같은 패턴. |
| `scripts/verify-framework-instance-spot-contracts.sh`(공백 정규화 `\s+→' '` 적용 검사) → `06-framework-api.ko.md` | 10 | 10/10(정규화 비교 기준) | 없음 |
| 〃 → `12-spot-messaging.ko.md` → `03-spot-actor/02-spot-messaging.ko.md` | 7 | 4/7(정규화 비교 기준) | **3개 깨짐**: "Spot direct call에 `Instance intent`"(새 문서는 `[Instance intent](../01-glossary.ko.md#instance-intent)` 링크 마크업으로 대체 — 백틱 코드가 아님), "target Spot이 존재하지 않으면 `NotFound`로 끝난다."(새 문서는 문장 순서가 바뀜: "...가 없는데 target Spot이 존재하지 않으면 `NotFound`로 새로 준비하라는..."), "최초 application message와 Spot 생성·reply에 필요한 정보를 하나의 전달 단위에 함께 넣는다."(새 문서는 "activation envelope로 함께 전달"로 재서술) |
| 〃 → `16-spot-address-messaging.ko.md` → `03-spot-actor/06-spot-address-messaging.ko.md` | 9 | 6/9(정규화 비교 기준) | **3개 깨짐**: "## 3. User Spot Create와 GetOrCreate"(새 heading: "## 3. User Spot 명시적 생성 — Create와 GetOrCreate"), "## 4. Direct message로 Instance Spot 생성을 허용하는 방법"(새 heading: "## 4. Cold activation — message로 Instance Spot을 처음 만드는 방법"), "생성 권한을 얻은 target만 자신을 owner로 기록하고 factory를 실행한다."(새 문서는 마침표 없이 문장이 이어짐: "...factory를 실행하며, 그 Spot이 처리하는...") |
| 〃 → `15-spot-actor.ko.md` → `03-spot-actor/05-spot-actor-membership.ko.md` | 7 | 6/7(정규화 비교 기준) | **1개 깨짐**: "Instance Spot은 source가 first-message activation envelope를 후보 target에 먼저 제출하고 target activation registry가 `Reserve`를 호출한다." — 새 문서에서 해당 문구를 찾지 못함(재서술 또는 이동 여부 미확인, 표본 검색 범위 안에서는 부재). |
| 〃 → `21-location-runtime.ko.md` → `05-location-relocation/01-location-runtime.ko.md` | 8 | 4/8(정규화 비교 기준) | **4개 깨짐**: "Instance Spot은 별도 생성 API를 사용하지 않는다.", "Instance Spot 요청임을 표시했고 Spot이 없을 때만, message를 받은 target node가 Spot을 만든다.", "동시에 여러 target이 시도해도 성공한 하나만 factory를 실행한다.", "Record가 없을 때만 최초 message를 Relocation Store에 저장하고 `Creating` record와 수용 공간을 함께 확보한다." — 모두 새 문서에서 정확히 같은 문구를 찾지 못함(주제 자체는 다뤄지나 표현이 재서술된 것으로 보임, 대체 위치는 이번 라운드에서 특정하지 못함). |

**요약**: `verify-framework-submit-api.sh`와 `verify-framework-instance-spot-contracts.sh`는 지금
당장은 옛 경로 파일(`05-async-execution-policy.ko.md`, `06-framework-api.ko.md`,
`12-spot-messaging.ko.md`, `16-spot-address-messaging.ko.md`, `15-spot-actor.ko.md`,
`21-location-runtime.ko.md`)을 직접 열어 검사하므로 깨지지 않는다. 이 라운드가 확인한 것은
"옛 문서가 삭제되고 이 needle들을 새 문서로 옮겨 검사하도록 스크립트를 갱신할 경우" 그대로
통과하는지이며, 위에서 표시한 **11개 needle**(spot-messaging 3 + spot-address-messaging 3 +
spot-actor-membership 1 + location-runtime 4)은 그 시점에 실패한다.

## 요약

- 불일치 N건: 0건(이 라운드 표본에서 cpp 구현이 스펙 수치·닫힌집합과 어긋나는 사례는
  발견하지 못함). 문서 자체의 내부 모순(G19, G20)은 cpp 관찰로 어느 쪽 표현과도 다르거나
  cpp가 한쪽 이름을 따르는 사실만 보고했으며 "cpp가 스펙을 어겼다"는 판정은 아니다.
- 스펙 미정 N건: 0건(이번 표본에서 새로 발견한 것은 없음 — G18 관련 3축 정확값은 스펙이
  의도적으로 미정으로 남긴 것이며 cpp 값(64/1MiB/2ms)만 보고했다).
- 판단 불가 N건: 8건 — (1) Typed Rejected vs ErrorKind.Rejected 구분 경로, (2) Actor Join
  request당 1 MiB/합계 8 MiB 상수, (3) Core/Application job queue profile 기본값 `Balanced`
  리터럴, (4) ClientServer probe가 RouteMesh와 동일 `service_liveness_registry_t` 인스턴스를
  쓰는지, (5) Host relocation interruption 1초 elapsed 실측 테스트, (6) fanout 15초 record
  timeout이 관찰되는 public status 노출 지점, (7)(8) runtime-monitoring/runtime-metrics의
  public 표면 부재 항목(endpoint·owner lease 등, metric label 부재) 전수 확인.
- 그 외: TEST 섹션에서 `verify-framework-instance-spot-contracts.sh`의 needle 41개 중
  **11개**가 대응하는 새 문서에서 정규화 비교로도 발견되지 않음(표현 재서술·heading 변경·
  링크 마크업 전환이 원인으로 보임) — 스크립트가 옛 경로를 참조하는 동안은 통과하지만,
  경로를 새 문서로 옮기면 즉시 깨진다.

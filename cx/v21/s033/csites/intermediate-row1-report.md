**중간 결과: row 1의 실제 public 등록 경로 binding 누락을 보완 중이다. 아래 after 수치는 보완 전 측정이며 최종 결과가 아니다.**

# C++ request sites — #1639 / #3867

승인된 11개 행 구현 및 검증 완료. 기준 `0cd1eac0c5fb367d0373f882094363a556b47478`, branch `perf/3867-cpp-request-sites`. 변경 파일은 `framework/languages/cpp/` 아래 57개다. GitHub, sync, Core C header 읽기, commit/push/branch 조작, sub-agent 사용 없음.

## 행별 변경과 결정 소유

경로는 cpp 루트 기준이다. 변경 전/후의 판정 위치 수는 의미상 같은 결정을 내리는 위치를 센다. allocation 수나 helper 호출 수를 규칙 수로 세지 않는다.

| 행 | 변경 file:line | 변경과 판정 위치 전 → 후 |
|---|---|---|
| R-a | [framework/src/runtime/stateful/public_host_runtime.cpp:7446](/root/worktree/zlink-3867h/framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:7446)<br>[framework/src/runtime/mesh/raw_mesh_node_owner.cpp:4211](/root/worktree/zlink-3867h/framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:4211) | 기존 hosted 역할에서 dispatch/wait 구현을 선택. standalone queue 재조회 두 위치 → 0; mailbox binding 권한 1 → 1. |
| R-b | [framework/src/runtime/mesh/mesh_node_host_service.cpp:3708](/root/worktree/zlink-3867h/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:3708)<br>[framework/src/runtime/spots/spot_runtime.cpp:15399](/root/worktree/zlink-3867h/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:15399) | decode 및 Spot 내부 Node/Channel 명령 판정까지 local terminal 유지. 기존 deferral branch에서만 승격. terminal 소유자 1 → 1. |
| 1 | [framework/include/zlink/framework/contracts/codecs/serializer.hpp:465](/root/worktree/zlink-3867h/framework/languages/cpp/framework/include/zlink/framework/contracts/codecs/serializer.hpp:465)<br>[framework/src/runtime/mesh/mesh_node_host_service.cpp:2628](/root/worktree/zlink-3867h/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:2628) | freeze 이후 기존 등록 plan에 serializer descriptor 연결. registry 선택 1 → 1; 동적 send cache 유지. |
| 2 | [framework/src/runtime/messaging/envelope_codec.hpp:34](/root/worktree/zlink-3867h/framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.hpp:34)<br>[framework/src/runtime/messaging/envelope_codec.cpp:1091](/root/worktree/zlink-3867h/framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:1091)<br>[framework/src/runtime/mesh/mesh_record_dispatcher.cpp:47](/root/worktree/zlink-3867h/framework/languages/cpp/framework/src/runtime/mesh/mesh_record_dispatcher.cpp:47) | retained header의 unescaped text는 view, escaped/public 복사본은 소유. canonical/SAX validation 소유자 1 → 1. |
| 3 | [framework/src/runtime/protocol/service_wire_codec.hpp:116](/root/worktree/zlink-3867h/framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.hpp:116)<br>[framework/src/runtime/protocol/service_wire_codec.cpp:2926](/root/worktree/zlink-3867h/framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:2926) | multipart discriminator에서 고정 metadata 유도, byte payload만 임의 text 소유. profile 판정 소유자 1 → 1; 같은 tuple predicate 3곳 → 1곳. |
| 5 | [framework/src/runtime/spots/spot_runtime.cpp:6612](/root/worktree/zlink-3867h/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:6612)<br>[framework/src/runtime/protocol/service_wire_codec.cpp:3042](/root/worktree/zlink-3867h/framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:3042) | accepted record를 borrow, native body retention만 유지. replay source에서 이동하지 않음. accepted-record 소유자 및 body validation 각 1 → 1. |
| 6 | [framework/src/runtime/mesh/mesh_node_runtime.cpp:4430](/root/worktree/zlink-3867h/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:4430)<br>[framework/include/zlink/framework/contracts/dispatch/request_task.hpp:130](/root/worktree/zlink-3867h/framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/request_task.hpp:130) | descriptor를 기존 request capture resource에 배치. 1 KiB body는 borrow. physical decode/delivery 소유자 1 → 1. |
| 7 | [framework/src/runtime/channels/route_packet_dispatcher.cpp:416](/root/worktree/zlink-3867h/framework/languages/cpp/framework/src/runtime/channels/route_packet_dispatcher.cpp:416)<br>[framework/src/runtime/channels/route_handler_invoker.hpp:12](/root/worktree/zlink-3867h/framework/languages/cpp/framework/src/runtime/channels/route_handler_invoker.hpp:12)<br>[framework/src/runtime/diagnostics/dispatch_options_access.hpp:20](/root/worktree/zlink-3867h/framework/languages/cpp/framework/src/runtime/diagnostics/dispatch_options_access.hpp:20) | context/header/completion을 기존 invocation frame으로 이동. startup immutable 설정 수명 공유. reply submit과 turn/terminal 소유자 각 1 → 1. |
| 10 | [framework/src/runtime/channels/channel_reply_writer.cpp:12](/root/worktree/zlink-3867h/framework/languages/cpp/framework/src/runtime/channels/channel_reply_writer.cpp:12) | reply header는 completing request text를 view. 동일 writer/validation/submit. reply 형성 소유자 1 → 1. |
| 12 | [framework/src/runtime/messaging/client_call_codec.cpp:34](/root/worktree/zlink-3867h/framework/languages/cpp/framework/src/runtime/messaging/client_call_codec.cpp:34) | deadline text는 request header 내부 고정 20바이트. absolute deadline 소유자 1 → 1. |
| 17 | [framework/src/runtime/foundation/operation_registry.cpp:401](/root/worktree/zlink-3867h/framework/languages/cpp/framework/src/runtime/foundation/operation_registry.cpp:401)<br>[framework/include/zlink/framework/contracts/dispatch/request_task.hpp:133](/root/worktree/zlink-3867h/framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/request_task.hpp:133) | 기존 request block에 completion item 배치, standalone callback은 기존 heap 경로 유지. C1 take / C2 index / completion dispatcher 각 1 → 1. |

소유 계층: 기존 Framework codec/registration, accepted record, invocation frame, operation registry. binding/Core의 retry·연결·admission 결정을 추가하지 않았다.

Spec: `01-submit-and-completion` §9–11(C1 once-only terminal, C2 callback 비보유, 별도 completion turn), `08-messaging-hot-path` §4(record별 scope/실행, reusable construction plan), `02-handler-turn-and-execution-gate` §1–3/§13(기존 turn/gate 및 infrastructure 진행)을 유지했다. payload ownership/codec와 wire framing 검증도 유지했다. Spec 변경 없음.

교차언어 대조: .NET `ZLinkCompletionDispatcher`, Java/Kotlin `ZLinkServiceCompletionDispatcher`, Node `queueMicrotask`의 별도 completion 실행을 확인했다. C++의 동일한 turn 경계를 유지하며 다른 언어의 동작을 바꾸는 변경은 없다. 네 언어 실행 검증은 이 작업 범위가 아니다.

변경 분류: R-a/R-b **A(계약 적응)**, 나머지 9행 **B(기존 비용 결함)**. 사용자 승인한 저장 표현/수명 변경 범위다. terminal/turn 판정자를 추가하지 않았다.

수정 전/후 규칙 수: codec·C1 terminal·completion turn의 판정자 각 1 → 1, hosted 경로의 standalone 재판정 위치 2 → 0, profile tuple 중복 판정 위치 3 → 1.

기존 640바이트 capture arena는 유지했다. 미사용 `fail_public` capture 32바이트를 제거하여 Node capture 608바이트 + descriptor 32바이트가 들어간다. completion 전용 slot 512바이트는 같은 request allocation 안에 두며 Linux item은 384바이트, 크기/정렬 static assertion으로 제한한다. 새 flag·counter·queue·pool·retry·thread 없음. C1 map node, dispatcher post, task mutex, socket/lifecycle locks, invocation coroutine/scope 등 keep 행은 유지한다.

## Count 조건과 process total

csite2 interceptor를 byte 그대로 복사하여 사용했다. `redirect.so`는 기존 count 파일의 open 경로만 이 job으로 보낸다(할당·lock interception 변경 없음). 원본 harness/benchmark 소스는 수정하지 않았다. `measure.py`의 배치 경로 치환과 SHA 기록은 run 디렉터리에 남겼다. Throughput은 같은 launch/warmup/active/workload에서 interceptor와 count snapshot만 끈다.

CPU source 0–7 / target 8–15, 1 KiB, warmup 30 s / active 5 s, `flock -o /root/jobs/fx/cx/measure.lock`. Before/after 동일 compiler/dependencies/test-probe 설정. 실행 중 MemAvailable ≥12 GiB 검사를 통과했다. 전용 메모리 계측은 수행하지 않았다.

Count 완료 요청 before **188,008**, after **187,125**. 제출=완료=target active 수신, errors/inFlight 0, post/run 일치. `validation.log`의 의도적 completed+1 및 completion_post−1 오염 자료는 거부됐다.

| Process / role | locks/request 전 | 후 | heap/request 전 | 후 |
|---|---:|---:|---:|---:|
| source / 0 | 10.535275 | 10.307559 | 2.687327 | 2.712534 |
| source / 1 | 36.425216 | 36.506971 | 17.927721 | 13.908735 |
| source / 2 | 46.890297 | 44.282827 | 9.608565 | 9.366878 |
| source / 3 | 6.003090 | 6.003340 | 7.002941 | 6.003228 |
| **source total** | **99.853879** | **97.100697** | **37.226554** | **31.991375** |
| target / 0 | 29.537615 | 29.457224 | 35.755744 | 27.790557 |
| target / 2 | 35.641670 | 33.631503 | 8.509462 | 8.369860 |
| target / 3 | 0.000000 | 0.000000 | 0.000000 | 0.000000 |
| **target total** | **65.179285** | **63.088727** | **44.265207** | **36.160417** |
source 전체 heap/request 감소: 5.235179 (14.06%).

target 전체 heap/request 감소: 8.104790 (18.31%).

Role 1 caller, 2 receive, 3 completion workers, 0 other(target application workers 및 native I/O 포함). locks = mutex_lock + 성공한 trylock, condition-wait 재획득 제외. 정확한 process call count이며 request denominator는 drain 완료 수다.

## 변경 site의 샘플 추정치

원본 interceptor의 1/64 stack sample을 role별 실제 call 수에 비례시킨 값이다. 첫 Framework frame으로 배정한 exclusive attribution이며, stack inline/최적화로 정확한 소스 allocation 횟수와 다를 수 있다. 각 셀은 **locks / heap per request**. Inclusive stack 결과도 `*-analysis.json`에 보존하며 겹치므로 합산하지 않는다.

| 행 | source 전 | source 후 | target 전 | target 후 |
|---|---:|---:|---:|---:|
| R-a | 1.141 / 0.000 | 0.000 / 0.000 | 1.203 / 0.000 | 0.000 / 0.000 |
| R-b | 0.000 / 0.000 | 0.000 / 0.000 | 0.000 / 1.004 | 0.000 / 0.000 |
| 1 | 2.064 / 0.000 | 2.106 / 0.000 | 2.045 / 0.000 | 2.072 / 0.000 |
| 2 | 0.000 / 1.000 | 0.000 / 1.749 | 0.000 / 2.003 | 0.000 / 0.000 |
| 3 | 0.000 / 2.061 | 0.000 / 0.000 | 0.000 / 2.002 | 0.000 / 0.000 |
| 5 | 0.000 / 0.000 | 0.000 / 0.000 | 0.000 / 0.000 | 0.000 / 0.000 |
| 6 | 0.000 / 1.002 | 0.000 / 0.000 | 0.000 / 0.000 | 0.000 / 0.000 |
| 7 | 0.000 / 0.000 | 0.000 / 0.000 | 0.000 / 1.999 | 4.008 / 4.009 |
| 10 | 0.000 / 0.000 | 0.000 / 0.000 | 0.000 / 1.006 | 0.000 / 0.000 |
| 12 | 0.000 / 0.986 | 0.000 / 0.000 | 0.000 / 0.000 | 0.000 / 0.000 |
| 17 | 1.010 / 1.989 | 1.027 / 0.992 | 0.000 / 0.000 | 0.000 / 0.000 |
before/source active sample 보존: 402,686/402,686 (100.00%). ring 초과 시 보존된 active suffix로 site를 추정하며 process total은 전체 active 구간이다.

before/target active sample 보존: 321,497/321,497 (100.00%). ring 초과 시 보존된 active suffix로 site를 추정하며 process total은 전체 active 구간이다.

after/source active sample 보존: 377,436/377,436 (100.00%). ring 초과 시 보존된 active suffix로 site를 추정하며 process total은 전체 active 구간이다.

after/target active sample 보존: 290,179/290,179 (100.00%). ring 초과 시 보존된 active suffix로 site를 추정하며 process total은 전체 active 구간이다.


Row 5는 replay 전용 경로로 본 steady-state workload의 sample은 0이며, allocation 제거는 accepted-body unit 검사(0 heap, 동일 native 주소)로 확인한다. Source의 owning header decode는 유지한다. R-b exclusive 값은 dispatch_application 직접 할당만 잡고 전체 subtree를 제거 비용으로 계산하지 않는다.

source PMR upstream sampled heap/request: 0.000000 → 0.000000.

target PMR upstream sampled heap/request: 0.000000 → 0.000000.

프로세스 전체 heap 감소와 request arena 양성/음성 비교(요청 resource 0, ordinary resource 1)를 함께 사용했다. 삭제한 site의 stack 이름만 바뀐 것으로 비용 절감을 판정하지 않는다. 모든 allocation 크기/메모리 총량이 감소했다는 주장은 하지 않는다.

## Throughput

단위 requests/s(또는 send-saturation messages/s), 각 3회. throughput 분모는 active close 완료 수/실제 active 시간이며 count의 drain denominator와 구별한다. 전 cell errors/abandoned/header failures 0, contaminated=false, drain bound hit=false.

| Stage / pattern | Framework 3 rounds | F median | Binding 3 rounds | B median | F/B |
|---|---|---:|---|---:|---:|
| before / request-backpressure | 51198.500, 53010.338, 52632.581 | 52632.581 | 259188.650, 249041.824, 259413.706 | 259188.650 | 0.203067 |
| before / send-saturation | 180385.457, 178989.068, 184169.410 | 180385.457 | 612769.755, 643358.511, 644514.004 | 643358.511 | 0.280381 |
| after / request-backpressure | 53329.186, 51205.678, 52481.749 | 52481.749 | 261754.656, 253170.697, 257440.663 | 257440.663 | 0.203860 |
| after / send-saturation | 185905.492, 186113.705, 182350.118 | 185905.492 | 625780.016, 628467.171, 649901.041 | 628467.171 | 0.295808 |

원본 harness의 자동 request window(`request_window=0`, peak 약 1,026)를 전후 동일하게 유지했다. Binding은 같은 unchanged 실행 파일이다. 3회 중앙값 비교이며 통계적 유의성을 주장하지 않는다.

## Tests와 logs

- Full build: `build-complete-final.log`, `build-parser-static-final.log` — 지정한 `flock -s … cpp-build-slot.sh cmake --build --preset linux-ninja-release --parallel 3 -- -k 0`.
- Unit/contract: `test-full-final.log`, `test-full-detail.log` — 지정한 label gate 전체.
- Format: `format-final.log` — repo root에서 `scripts/format/format.sh --check cpp` 통과.
- 수정 개별 확인: `test-cp51-fix.log`, `test-receive-wrapper-fix.log`, `test-idle-wrapper-fix.log`, `test-native-body-lifetime.log`.
- Focused: `test-focused-final.log`, `test-focused-detail.log`, `test-codecs-final.log`. 기존 notification 3/native copy 2 guard 유지(기준본 별도 실행 `baseline-test.log`도 동일).
- Interceptor 보정: `calibration.log` — malloc/aligned_alloc/mutex_lock/cond_signal 각각 1.
- Provenance: `provenance.json`, `provenance.log` — 기준 HEAD/branch, 허용된 변경 경로, binding/interceptor 동일 SHA, after-perf archive와 최종 build archive 일치.
- Count: `before-counts.log`, `after-counts.log`, 각 `*-counts-framework-request-backpressure-1/`의 raw count/maps/stats. 분석: `before-analysis.json`, `after-analysis.json`. 검증: `validation.log`, `validation.json`.
- Throughput: `before-throughput.log`, `after-throughput.log`, 각 `*-throughput-*` run의 원본 results/stats/launch 자료 및 harness SHA.

수명/terminal 검사:

| 행 | 검사와 실패 감지 |
|---|---|
| R-b / 7 | `CsitesLocalTerminalAndInvocationSurviveAllRequestOutcomes`: suspend 중 terminal capture alive, reply·timeout·cancel·shutdown·throw 후 reply 1/terminal 1, scope 생성/파괴 일치. `CsitesInternalRequestKeepsTerminalThroughSynchronousRejection`: 내부 Node 명령의 async 거절 후 sync error reply가 terminal보다 먼저 이루어지는지 검사. |
| 5 | `CsitesAcceptedBodyRetainsNativeStorageAfterRecordRelease`: native 주소 동일, record 파괴 뒤 retained body alive, body 해제 뒤 source 해제, decoder heap 0. 위 5경로 invocation 검사는 외부 native body의 weak reference로 suspend 중 생존과 정리 후 해제를 확인한다(`test-native-body-lifetime.log`). |
| 6 / 17 | `CsitesRequestCompletionKeepsStorageUntilEveryTerminalTurn`: 실제 registry reply/expire/cancel/shutdown/throw 경로, callback 전 marker alive, terminal 1, late complete/cancel 거절, retained task와 C2 tombstone을 둔 채 capture 해제. descriptor decode를 callback 안에서 수행한다. `CsitesReplyDescriptorsUseRequestStorageUntilProjectionReturns`: request resource heap 0 대 ordinary heap 1. |
| 1 / 2 / 10 / 12 | 서로 다른 frozen registry codec 결과와 wrong content-type 거절, header source 이동/복사/자기대입과 보유 수명, reply view, inline deadline 이동/복사. |

첫 전체 gate의 3개 실패(`test-first-gate.log`)는 coroutine capture의 suspend 후 접근 1개와 hosted 함수 관측 wrapper 누락 2개였다. codec plan을 suspend 전에 지역 shared 소유로 유지하고, wrapper를 실제 hosted 함수와 반환형에 맞춘 뒤 재검증했다. 검사 조건과 timeout은 유지했다. 실패한 초기 빌드는 호출부 적응 오류를 수정한 뒤 재빌드했다. 초기 allocation 검사 실패는 writer의 불필요 string materialization 및 header native-copy 원인을 수정했다. assertion 완화나 retry/timeout 증가 없음. 빌드 중 memory 하한 미달 때는 중단 후 단일 build로 재개했고, 최종 검사 및 모든 측정은 하한을 충족했다. 남은 실패 없음.

리팩토링 점검: 성능 11건, POSDDD 0건, 불필요 코드 1건 발견. 고친 항목: 승인 11개 행 및 미사용 fail_public capture 제거. 넘긴 항목 없음.

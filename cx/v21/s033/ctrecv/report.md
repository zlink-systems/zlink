# #3868 C++ Framework target 수신 경로

목표는 target user CPU 2.3 µs/message와 unpaced Framework/binding 처리량 비율 0.6이다. 기준은 875d773ef9의 source 변경을 포함한 현재 branch다. Main 비교 바이너리는 csrc/build-main을 사용한다. 네 단계를 구현하고 측정했으나 목표를 충족하지 못했다. 최종 target user 중앙값은 6.775 µs/message로 예산을 4.475 µs/message 초과했고 F÷B 중앙값은 0.422로 0.6에 0.178 미달했다.

## 결정 소유와 실행 자원 전환

S08, S04, S02, S01, S05는 framework/doc/framework/common/spec/server/01-execution의 messaging-hot-path, application-job-queue-and-backpressure, handler-turn-and-execution-gate, submit-and-completion, payload-ownership-and-codec 문서다. 아래 C++ 경로는 framework/languages/cpp/framework/src/runtime 기준이며 위치는 변경 전이다.

| 결정 | 계약 | 현재 C++ 위치 | 변경 뒤 소유자 |
|---|---|---|---|
| Ordinary receive 전 permit 예약과 source FIFO | S04 §3 | dispatch/application_job_queue.hpp의 application_supply_slot_t; stateful/public_host_runtime.cpp:7546 | 기존 application queue와 supply slot |
| 회전의 socket·port 수명과 연속 receive | S08 §4.1 I0–I2, §4.4 | mesh/raw_mesh_node_owner.cpp:3380,3606; stateful/public_host_runtime.cpp:7546 | 기존 receive turn이 port 한 번 보관 |
| Service header 검증과 application owner 분류 | S08 §4.1 I3–I4 | mesh/raw_mesh_node_owner.cpp:3647,4147; stateful/public_host_runtime.cpp:8123,8347,8411 | ingress의 기존 codec·분류 결과를 record와 이전 |
| Queue 수락·FIFO·ready 집합·W5 | S04 §3, S08 §4.3 | mesh/service_mailbox.cpp:78,174,409; mesh/mesh_node_host_service.cpp:3842 | 기존 mailbox와 scheduler; 정책 변경 없음 |
| Claim credit와 정확히 한 번 settlement | S04 §3, S02 §2–3 | mesh/service_mailbox.cpp:174,365–430; stateful/public_host_runtime.cpp:739 | mailbox의 기존 owner queue 저장소와 count settlement |
| Envelope header 해석과 검증 | S08 §4.1–4.3, S05 §6 | messaging/envelope_codec.cpp:1232; mesh/mesh_node_host_service.cpp:113 | 기존 codec; 반복 가능한 header의 immutable cache |
| Route·typed deserializer·DI 생성 절차 | S08 §4.3 | channels/route_handler_registry.hpp:68–218; channels/route_packet_dispatcher.cpp:135,282; configuration/services.cpp:290 | 기존 registry entry와 scope 없는 prepared factory |
| Dispatch마다 새 scope·filter와 handler 공유 | S08 §4.3, Framework API §10–11 | channels/route_handler_invoker.cpp:38; configuration/services.cpp:254 | 기존 service_scope_t; dispatch 간 instance 공유 없음 |
| Handler 실제 시작 직전 permit 반환 | S04 §3, S08 W3 | channels/route_handler_invoker.cpp:29; contracts/detail/handler_invocation.hpp:58 | 기존 handler_entry와 invoke_prepared_handler |
| Handler 예외·reply·scope 정리·terminal | S01 §10–11, S08 W4 | channels/route_handler_invoker.cpp:51–112; mesh/mesh_node_host_service.cpp:278 | 기존 terminal owner; ready 결과는 같은 호출에서 완료 |
| 실행 자원 배치 | S08 §4.4, S02 §13 | mesh/mesh_node_host_service.cpp:2135,3842; dispatch/offload_executor | 기존 executor; 수정하지 않음 |

현재 전환 수는 수신 역할 인계 성공 시 0회, 기존 worker 게시 시 1회다. 변경 뒤도 0회 또는 1회다. Coroutine 호출·동기 lane 접근·함수 위임은 실행 자원 전환으로 세지 않는다. Claim에서 handler까지 mailbox 하나가 owner queue이며 별도 record task나 두 번째 runnable queue를 추가하지 않는다.

cplanr 제안 중 hosted/standalone 선택은 public_host_runtime.cpp:7505와 raw_mesh_node_owner.cpp:4272에 이미 반영돼 있다. 기존 terminal 직접 이전도 mesh_node_host_service.cpp:3790–3818과 spots/spot_runtime.cpp:15716에 반영돼 있다. 같은 수정을 다시 성능 개선으로 계산하지 않는다.

교차언어 대조: .NET Runtime/Messaging/ZLinkEnvelopeCodec.cs:377–411,1385–1428은 bytes 비교, 64항목, 1024바이트와 correlation/deadline/flow/metadata 제외 규칙을 갖는다. .NET Runtime/Handlers/ZLinkHandlerDispatcher.cs:96,150은 dispatch마다 scope를 만든다. Java의 runtime/channels/ZLinkChannelHandlerInvoker.java:285,344는 route handler와 filter 호출을 기존 dispatcher에 연결하며 Kotlin은 이 runtime을 공유한다. Node packages/framework/src/runtime/channels/channel-dispatch-services.ts:167,188은 handler scope에서 resolve하고 channel-dispatch-pipeline.ts:355,391은 handler 시작에 permit을 반환한다. 표현과 allocation만 바꾸며 언어별 관측 계약을 바꾸지 않는다. 변경 분류는 A(기존 계약 적응)다.

두 대안을 비교했다. Cache와 별도 route 상태를 각각 유지하면 같은 header의 선택 결과를 두 곳에서 보관한다. 선택한 대안은 bounded immutable cache entry에 header와 기존 registry entry를 함께 보관하고 scope instance는 보관하지 않는다. Claim도 새 queue를 만드는 대신 기존 queue의 저장 표현을 바꾼다.

## 단계별 변경과 측정

기준판의 perf 없는 같은 rate 3회 target user는 8.587–9.089 µs/message이며 중앙값은 8.690이다. 모든 cell의 submitted, completed와 target received가 일치했고 error, abandoned, header validation failure와 target rejection은 0이다. 원본은 raw/baseline/paced/와 raw/cpu-summary.json에 있다. 별도 profile 3회 target user는 8.524–8.736이며 일반 CPU 측정과 구별한다.

기준판 재링크에는 csrc2 측정 시의 바이너리 해시가 모두 같은 기존 C++ 산출물을 재사용했다. ce1b779323 대비 C++ diff를 대조하면 차이는 envelope_codec.cpp 하나이며, 875d773ef9의 해당 object로 archive member를 바꾼 뒤 같은 Release 링크 명령으로 client와 target을 재링크했다. C++의 공유 service-wire 자산은 동일하다. 추가된 공통 runtime 파일은 C++ 빌드에서 사용하지 않는 etcd proto다. 근거는 raw/baseline-cpp-comparison.json, raw/bootstrap-baseline.json과 raw/baseline-codec-preservation.json이다.

### 단계 (a)

동일한 header bytes와 registry table을 비교해 immutable header와 기존 typed route entry를 함께 재사용한다. .NET과 같은 FIFO 64항목·최대 1024바이트를 사용한다. Correlation, deadline, flow와 metadata가 있으면 기존 decoder로 간다. Scope와 handler instance는 cache에 넣지 않는다. 주요 변경은 channels/route_handler_registry.cpp의 prepare_header, messaging/envelope_codec.cpp의 repeatable 판정, mesh/mesh_node_host_service.cpp의 decode_mesh_record_header_impl과 channels/route_packet_dispatcher.cpp의 단일 resolve다. 정확한 최종 위치는 마지막 표에 적는다.

같은 rate 교차 3회에서 875d773ef9 기준판 target user 중앙값은 8.615(8.384–8.692), 단계 (a)는 7.770(7.760–7.903) µs/message다. 중앙값 절감은 0.845이며 두 범위는 겹치지 않는다. 따라서 단계 (a)를 유지한다. 원본은 raw/step-a/paced, 누적 diff는 step-a.diff다. 별도 profile 3회의 header decode·cache 비용은 0.070(0.026–0.074)이며 기준판은 0.651(0.527–0.712) µs/message다. 이 부품의 중앙값 절감은 0.581, 두 범위의 차이는 0.453–0.686이다. Header 예산 0.15는 이 분류에서 충족했다.

Profile은 동일한 user CPU period를 실제 CPU 시간으로 정규화했다. Header 구조체를 mailbox record와 함께 이동하는 비용은 decode가 아니라 그 이동을 수행하는 상위 모듈에 귀속했다. 처음의 광범위한 header-storage 분류도 raw/profile-summary-broad-header-storage.json에 남겼다. Native message 생성·복사·close도 가장 가까운 Framework 호출 소유자에 귀속했다. Native라는 이유로 모두 port receive로 세지 않았다. Native-first 분류는 raw/profile-summary-direct-header-native-first.json에 보존했다. 같은 최종 기준을 baseline과 모든 단계에 적용하며, 제공된 ctgt의 분류값과 직접 뺄셈하지 않는다. 단계 (a)의 port는 1.938(1.819–2.560), classify·queue·claim은 1.822(1.786–1.874), 나머지 receive dispatch는 0.436(0.399–0.473), route·DI·invoke·terminal은 3.532(2.665–3.581)이다. 이 부품들의 차이는 측정 범위 안이므로 절감으로 판정하지 않는다.

새 계약 테스트와 기존 관련 테스트 7개가 8 CPU·80% 부하에서 통과했다. raw/step-a-fixed-tests.log와 status.json을 보존했다. 첫 실행은 새 fixture가 일반 C++ 예외를 Framework error로 가정해서 실패했다. 실제 기존 경로처럼 원래 exception_ptr의 타입과 메시지를 검사하도록 fixture를 수정했고 runtime 예외 처리는 바꾸지 않았다. 첫 실패도 raw/step-a-tests.log에 남겼다.

### 단계 (b)

Mesh record dispatcher를 기존 route packet dispatcher에 합쳤다. Hosted callback은 registration에서 dispatcher를 만들고 owner drain은 한 번 만든 설정을 재사용한다. Ordinary Node/Channel packet은 기존 Spot 내부 protocol 분류표로 확인한 뒤 곧바로 route invoker로 보낸다. 내부 packet은 기존 Spot 소유자에 남겼다. mesh_record_dispatcher.cpp와 hpp를 지웠다.

Send completion closure, reply lambda와 두 flow-entry closure를 구체적인 completion 값과 기존 reply token으로 바꿨다. Filter terminal은 기존 registry의 같은 filter chain을 사용하며 empty chain에서 std::function을 만들지 않는다. 이미 완료된 결과는 같은 호출에서 scope와 terminal을 정리한다. 완료되지 않은 결과만 기존 await 경로로 넘긴다. 결과의 native message를 다시 복사하지 않고 기존 task_result_access_t로 빌린다. 새 scope는 매 dispatch마다 만든다. 재사용하는 것은 빈 저장소이며 pending 작업은 그 저장소의 cache 참조를 풀어 실제 작업의 수명이 정리를 소유하게 했다.

Ready dispatch의 공통 invoker coroutine state는 1개에서 0개다. Typed async handler adapter와 비어 있지 않은 filter의 기존 continuation은 남아 있다. Typed payload와 context를 참조하는 pending handler의 수명을 보존하는 경로이며, 이 adapter까지 frame 0개라고 주장하지 않는다. 단계 (a)의 라이브러리에 같은 0-frame 검사를 연결하면 실제 1개가 측정되어 실패한다. raw/step-a-frame-negative.log가 음성 대조다. 단계 (b)의 양성 결과는 raw/step-b-fixed-tests-gtest/test_cpp_framework_receive_budget_contract.xml에 있다. 실행 자원 전환은 계속 0회 또는 1회다.

관련 unit/contract 9개를 8 CPU·80% 부하에서 통과했다. raw/step-b-fixed-tests.log와 XML에 scope 분리, filter 전 permit 유지, handler 시작 직전 반환, 동기·ready task·suspend의 성공과 예외, terminal 1회, 중첩된 pending 작업의 payload/context 수명 결과가 있다. 첫 실행의 기존 permit 검사 2개는 이동 전 header의 짧은 문자열을 binding이 참조한 결함을 잡았다. Override는 안정된 설정/record 주소를 참조하고 header channel은 현재 header 소유자에서 읽도록 고쳤다. 기존 assertion을 바꾸지 않았으며 첫 실패는 raw/step-b-tests.log에 보존했다.

누적 변경은 step-b.diff, 바이너리 해시는 raw/step-b-binary.json에 있다. 같은 rate 교차 3회 target user 중앙값은 875 기준판 8.508(8.462–8.800), 단계 (a) 7.785(7.718–7.823), 단계 (b) 6.795(6.655–6.804) µs/message다. 단계 (a) 대비 0.990, 875 기준판 대비 1.713을 절감했다. 범위가 겹치지 않아 단계 (b)를 유지한다. 원본은 raw/step-b/paced다. 아직 총 예산을 4.495 µs/message 초과한다.

별도 profile의 부품별 결과는 다음과 같다.

step-a port_core_binding: 1.938 (1.819–2.560)
step-a route_di_invoke_terminal: 3.532 (2.665–3.581)
step-a classify_queue_claim: 1.822 (1.786–1.874)
step-a receive_dispatch_rest: 0.436 (0.399–0.473)
step-a handler_bench: 0.337 (0.325–0.362)
step-a envelope_header: 0.070 (0.026–0.074)
step-a unattributed: 0.004 (0.000–0.010)
step-b classify_queue_claim: 1.801 (1.767–1.889)
step-b port_core_binding: 1.913 (1.793–2.349)
step-b receive_dispatch_rest: 0.403 (0.298–0.481)
step-b handler_bench: 0.278 (0.263–0.310)
step-b route_di_invoke_terminal: 2.309 (1.952–2.638)
step-b envelope_header: 0.072 (0.061–0.101)

### 단계 (c)

기존 application batch 안에 claim을 포함하고 같은 제어 블록을 alias한다. 별도 claim make_shared와 record_bytes vector를 없앴다. Claim의 slot pointer와 settlement bit는 같은 PMR 저장소를 사용하며 receive batch 64개는 내부 저장소에 들어간다. Record별 atomic_bool 배열을 64비트 settlement로 바꾸고 기존 remaining counter를 완료된 word 수로 바꿨다. 각 credit은 bit 한 번으로 정산하고 마지막 bit가 word를 완료할 때만 counter를 감소시킨다. Message별 counter 갱신과 별도 settles_last helper를 없앴다. outstanding 조회는 같은 bit에서 계산한다. 큐의 FIFO·phase·ready 목록·worker·wake 판정은 바꾸지 않았다.

Ingress가 읽은 service header와 수락한 byte 비용을 canonical record에 두고 claim과 worker가 재사용한다. Mesh가 사용하지 않는 576바이트 envelope header는 client/server payload에서만 소유하도록 바꿨다. Client/server의 기존 producer와 consumer도 같은 소유 표현으로 맞췄다. 새 queue·lock·제어 flag·제어 counter는 없다. Byte 비용은 지운 record_bytes의 같은 사실을 원본으로 옮긴 것이다.

단계 (c)의 첫 부하 실행에서 claim 비용 상한이 batch 저장소 171,376바이트 재할당을 잡았다. dispatch_application_owner의 지역 claim alias가 재사용 검사 때까지 남은 것이 원인이었다. Batch가 소유한 뒤 지역 alias를 reset해 같은 저장소를 재사용하도록 고쳤다. 기존 비용 assertion과 주입 검사 조건을 바꾸지 않았다.

수정 뒤 부하 실행은 12개 중 11개가 통과했다. 새 header·scope·permit·exception·terminal·credit 계약과 기존 claim 비용·FIFO·reply 검사는 통과했다. 남은 실패는 ReceiveAllocations.AutomaticClientServerSendRecordHasFixedAllocationCount의 기대 23, 실제 20이다. Claim 제어 블록·두 vector·settlement 배열의 할당 네 개를 제거하고 client/server header 할당 하나를 추가해 순수하게 세 개 줄어든 결과이며, 구현에 맞춰 assertion을 바꾸지 말라는 지시를 따라 기대값을 유지했다. 이 실패를 통과로 처리하지 않았다. raw/step-c-fixed-tests.log, status.json, full-ctest.log와 XML에 전체 결과를 남겼다. 첫 실패는 raw/step-c-tests.log다.

Mesh record 크기를 줄이는 대안으로 inline optional header도 비교했다. 추가 할당이 없지만 record 최대 크기를 유지한다. 선택한 pointer 소유는 client/server에 header 객체 할당 1회를 추가하고, 공통 claim metadata 할당 4회를 제거한다. 지정된 성능 측정은 Mesh 수신 경로이며 client/server throughput은 측정하지 않았다.

같은 rate 교차 3회의 target user 중앙값은 875 기준판 8.303(8.213–8.577), 단계 (b) 6.820(6.774–7.001), 단계 (c) 6.595(6.380–6.690) µs/message다. 이전 단계 대비 0.225, 875 대비 1.708을 절감했다. 두 단계 범위가 겹치지 않아 (c)를 유지한다. 총 예산은 아직 4.295를 초과한다. 원본은 raw/step-c/paced다.

Unpaced send-saturation 3회는 main 174,720–182,304, 중앙값 181,104 message/s와 단계 (c) 257,081–261,579, 중앙값 260,602다. Main 대비 중앙값은 43.9% 높고 오류·유실은 0이다. 이 비교는 이전 단계와 875의 source 변경도 포함하므로 차이 전체를 (c)의 효과로 계산하지 않는다. Worker·wake·W5·ready-owner 규칙은 유지했다. 원본은 raw/step-c/unpaced다.

수정 뒤 warmed claim의 추가 할당은 0개/0바이트이며, 소유 callback 복사만 있는 경우 1개/96바이트다. 같은 경로에 24바이트 할당 하나를 주입하면 각각 1개/24바이트와 2개/120바이트가 되어 기존 주입 검사가 거부한다. 31개 record 추가에 따른 record별 추가 할당도 0이다. 결과는 raw/step-c-fixed-tests-full-ctest.log에 있다.

### 단계 (d)

Port 참조를 receive turn에 한 번 보관하고 receive_next는 그 참조를 빌린다. Record마다 lifecycle mutex를 다시 잡지 않는다. Raw port가 받은 parts의 header span과 byte 수를 한 번 기록하고, 수신·검증·queue charge가 같은 값을 사용한다. Flow가 frame을 바꾸면 기존 queue charge를 다시 계산한다. Socket lock과 route snapshot 정책은 유지했다.

이미 분류한 기본 service header는 enqueue_received에서 원본 record에 보관한다. Bound-session·multicast와 control의 조기 분기도 같은 commit 경로를 사용한다. Request-kind 분류는 handler_registry_runtime.hpp 하나로 합쳤다. Port 이동 뒤 span과 reply capability가 유지되는 검사를 추가했다. 부하 테스트는 13개 중 12개가 통과했다. Port snapshot·이동·reply, M6a와 기존 scope·permit·예외·terminal·FIFO 검사는 통과했다. 남은 실패는 (c)와 같은 할당 수 검사(23 기대, 20 실제) 하나다. raw/step-d-tests.log, status.json, full-ctest.log와 XML을 보존했다. 누적 변경은 step-d.diff다.

같은 rate 교차 3회의 target user는 875 기준판 8.490(8.437–8.661), 단계 (c) 6.501(6.485–6.645), 단계 (d) 6.618(6.501–6.781) µs/message다. 이전 단계 대비 중앙값은 0.117 증가했고 875 대비 1.872 절감했다. (c)와 (d)의 범위가 겹치므로 (d)의 CPU 절감은 확인하지 못했다. 기존 raw_received_bytes helper와 중복 request 분류 helper를 없애고 두 결정의 위치를 4곳에서 2곳으로 줄였으므로, 범위 안 효과는 코드가 단순해질 때만 유지하라는 기준으로 (d)를 유지했다. Kernel CPU와 source CPU 변화도 전체 표에 그대로 적었다. 단계 (d)의 target user는 예산을 4.318 초과한다. 원본은 raw/step-d/paced와 profiles다.

## 검증

모든 빌드는 linux-ninja-release, -O3, -DNDEBUG이며 -g는 사용하지 않았다. 공통 GCC 내부 메모리 회수 값은 expand=10, heap=32768이다. 단계 (c)·(d) production의 Spot 번역 단위에는 expand=3, heap=32768을 덧붙였다. Probe는 expand=1, heap=8192 시도도 메모리 보호선에서 중단돼 다른 C++ 빌드가 끝나기를 기다린 뒤 공통 값 10/32768로 빌드했다. 최적화 수준은 계속 -O3다. 공유 머신의 다른 큰 컴파일과 겹칠 때 가용 메모리 보호선에 걸려 적용한 컴파일 메모리 옵션이다. Runtime 옵션·최적화 수준·worker 수는 바꾸지 않았다. Configure와 build status에 명령, 최소 가용 메모리, 중단 여부와 du 결과를 남겼다.

단계별 관련 검증은 다음과 같다. 모든 테스트는 measure-shared.sh로 실행했고 stress-ng의 CPU 8개·80% 부하를 함께 걸었다. XML은 각 log 이름의 -gtest 디렉터리에 있다. 실패 로그와 수정 뒤 로그를 모두 보존했다.

| 단계 | 관련 CTest 결과 | 로그 |
|---|---|---|
| (a) | 7개 통과 | raw/step-a-fixed-tests.log, raw/step-a-fixed-tests-status.json |
| (b) | 9개 통과 | raw/step-b-fixed-tests.log, raw/step-b-fixed-tests-status.json |
| (c) | 12개 중 11개 통과, 고정 할당 수 검사 실패 | raw/step-c-fixed-tests.log, raw/step-c-fixed-tests-status.json |
| (d) | 13개 중 12개 통과, 같은 고정 할당 수 검사 실패 | raw/step-d-tests.log, raw/step-d-tests-status.json |

매 단계에서 fresh scope, owner FIFO·claim, handler 시작 전 permit 반환, 동기·ready·suspend의 성공·예외, terminal 1회를 검사했다. 기존 owner dispatch 검사는 대기 record 128개의 진입 순서와 다른 owner의 독립 완료를 확인한다. 테스트의 -UNDEBUG로 assertion이 활성화돼 있으며 근거는 raw/test-assert-flags.txt다. Cache는 동일 bytes hit, bytes·route 변경 miss, correlation·deadline·flow·metadata 제외, 1024바이트 경계와 FIFO 64항목 한도를 검사했다. Inline frame 검사의 이전 단계 음성 대조와 claim 할당 주입 검사의 양방향 결과도 위에 적었다.

최종 전체 unit·contract 실행을 위해 남은 테스트 바이너리를 빌드하고 있다. 모든 raw 출력은 raw/ 아래 보존한다.

## 최종 성능

최종 변경판 target user는 6.775 (6.724–6.912) µs/message이며, 예산 2.3을 4.475 µs/message 초과했다. 목표를 충족하지 못했다. 같은 최종 비교의 main 중앙값 9.567 대비 2.792 µs/message, 29.2% 감소했다. 이 비교에는 보존한 source 변경 875d773ef9도 포함된다. Source·target user/kernel 3회 전체 결과는 아래 CPU 표와 raw/final/paced에 있다.

| 판 | unpaced message/s, 5회 중앙값·범위 | F÷B, 5회 중앙값·범위 | 0.6 대비 중앙값 차이 | wall µs/message 초과분 |
|---|---:|---:|---:|---:|
| main | 184059 (179580–187332) | 0.302 (0.300–0.326) | -0.298 | 2.672 (2.480–2.762) |
| step-d | 255530 (250820–257334) | 0.422 (0.411–0.453) | -0.178 | 1.180 (0.960–1.226) |
| binding | 593756 (564324–625152) | 1.000 | 기준 | 기준 |

비율 자체는 µs 단위가 아니다. 마지막 열은 각 binding run 처리량의 0.6배를 충족할 때 필요한 wall time/message와 Framework 실측 wall time/message의 차이다. CPU user 예산 초과분과 구별한다. 비율은 같은 run 번호끼리 나눴으며 순서는 회마다 교차했다.

원본은 raw/final/unpaced와 raw/final/paced다. 모든 cell의 errors·abandoned·header validation failures·target rejected는 0이며 submitted·completed·target received가 일치했다.

## 수신 경로 중복 점검과 수

아래 위치는 최종 source의 framework/languages/cpp/framework/src/runtime 기준이다. 캐시 hit의 실행 비용과 source의 판정 위치 수를 구분했다. Core·binding 내부 mutex는 수정하지 않았고 이 표의 수에 넣지 않았다.

| 점검 항목 | 변경과 최종 위치 | 상태 |
|---|---|---|
| 반복 header의 decode·필드 이동 | channels/route_handler_registry.cpp:56, messaging/envelope_codec.hpp:168에서 immutable header 재사용 | hit에서 제거, miss 유지 |
| Send/request마다 route 재선택 | registry entry와 typed codec plan 재사용, channels/route_packet_dispatcher.cpp:256 | hit에서 lookup 제거 |
| Ordinary Node/Channel의 Spot 위임 | mesh/mesh_node_host_service.cpp의 ordinary_owner 분기에서 직접 route 호출 | 제거 |
| Mesh record dispatcher wrapper | mesh/mesh_record_dispatcher.cpp·hpp 삭제 | 제거 |
| Record마다 dispatcher 설정 객체 구성 | host registration 또는 owner drain에서 설정 | 제거 |
| Send completion std::function | channels/route_packet_dispatcher.cpp:294의 구체 completion | 제거 |
| Reply lambda | 기존 host reply token을 이전 | 제거 |
| Flow entry closure 두 개 | route_completion_t::entered에서 기존 tracer 사용 | 제거 |
| 공통 invoker의 ready coroutine | channels/route_handler_invoker.cpp:173에서 inline finish, :122는 suspend만 처리 | 제거 |
| 별도 claim 제어 블록·metadata 할당 | stateful/public_host_runtime.cpp:768, :8586의 기존 batch alias | hot claim에서 제거 |
| Record별 settlement 배열·counter 갱신 | mesh/service_mailbox.cpp:399의 bit·word count | 기존 count로 통합 |
| Header data·size와 byte 합산 반복 | backend/raw_route_port.cpp:505의 snapshot과 queue charge 재사용 | ordinary receive에서 제거 |
| Record별 port lifecycle mutex·참조 취득 | mesh/raw_mesh_node_owner.cpp:3381에서 turn 한 번 취득, :3608에서 빌림 | 제거 |
| Request-kind 중복 분류 | handlers/handler_registry_runtime.hpp:14 | 2곳에서 1곳으로 통합 |
| Context·route received 값의 구성·복사 | channels/route_packet_dispatcher.cpp:207, :244 | 유지, 추가 최적화 범위 |
| Typed async adapter·nonempty filter continuation | channels/route_handler_registry.hpp의 async overload와 handlers/handler_registry.cpp | payload·context 수명을 위해 유지 |
| Worker의 parts·native owner 구성 | stateful/public_host_runtime.cpp의 decode_application_parts 경로 | 유지 |
| Canonical queue record를 batch로 이전 | mesh/service_mailbox.cpp의 transfer_locked와 host batch | 유지 |

점검 18항목 중 14항목의 반복 작업·wrapper를 제거하거나 통합했고 4항목은 유지했다. 남은 비용은 같은 #3868의 후속 검토 대상이며 별도 Issue를 만들지 않았다.

규칙 수는 port 선택과 request-kind 두 결정의 판정 위치를 합쳐 4곳에서 2곳으로 줄었다. Permit 반환·terminal 정산·ready-owner/W5 정책의 소유자는 각각 기존 한 곳이며 바뀌지 않았다. Header cache는 등록 metadata만 보관하고 scoped instance를 보관하지 않는다. Claim별 byte vector는 원본 record의 수락 비용으로 옮겼고 message별 remaining counter는 word counter 하나로 대체했다. 저장소 helper는 기존 전용 TLS·acquire·settlement helper 7개를 공통 표현 5개로 줄였다.

수정 경계의 mutex 소유 객체는 lifecycle·port socket·port poller·mailbox 4종에서 4종이다. 일반 record의 lifecycle 취득은 1회에서 0회이며 turn에 1회 남는다. Socket·mailbox·poller lock 정책은 그대로다. 공통 ready invoker의 coroutine task state에 따르던 mutex 객체는 1개에서 0개다. Pending task·typed async adapter·filter와 기존 permit queue·supply·executor의 mutex는 유지했다. 새 실행 queue·lock·제어 flag·제어 counter는 추가하지 않았다.

Claim부터 handler 첫 명령까지 실행 자원 전환은 기존 receive-role handoff 경로 0회, worker 게시 경로 1회이며 변경 뒤도 같다. Worker 수·role 배치·wake·ready-owner membership·W5 backlog 규칙은 수정하지 않았다.

소유 계층은 Framework ingress·mailbox·handler registry·invoker다. Core·binding의 연결·재시도·errno·completion 결정을 옮기지 않았다.
Spec 근거는 S08 §4.1–4.4, S04, S02, S01 §10–11과 S05 §6이다.
교차언어 대조는 .NET header cache의 bound·bytes·제외 규칙과 .NET·Java/Kotlin·Node의 fresh scope·handler entry permit 계약이다.
변경 분류는 A(기존 계약 적응)다.

<!-- PARTS_TABLE -->
## 부품별 예산과 절감

단위는 µs/message다. 각 부품은 3회 별도 profile의 중앙값과 범위다. 절감 범위는 이전 판 최솟값−새 판 최댓값부터 이전 판 최댓값−새 판 최솟값까지다. 0을 포함하면 해당 부품의 절감을 확정하지 않는다. Handler·bench와 미분류 비용은 마지막 행에 포함해 총비용을 누락하지 않았다. 총 CPU의 유지 판정은 앞의 perf 없는 교차 측정을 사용한다.

| 단계 | 부품 | 실측 | 이전 단계 대비 절감 | 875 대비 절감 | 예산 | 중앙값 초과분 |
|---|---|---:|---:|---:|---:|---:|
| step-a | Core/binding와 port | 1.938 (1.819–2.560) | -0.074 (-0.770–+0.747) | -0.074 (-0.770–+0.747) | 1.45 | 0.488 |
| step-a | 분류·queue·claim과 나머지 receive dispatch | 2.258 (2.258–2.273) | +0.021 (-0.117–+0.148) | +0.021 (-0.117–+0.148) | 0.25 | 2.008 |
| step-a | Envelope header | 0.070 (0.026–0.074) | +0.581 (+0.453–+0.686) | +0.581 (+0.453–+0.686) | 0.15 | 0.000 |
| step-a | Route·DI·invoke·terminal과 나머지 | 3.904 (3.007–3.906) | -0.114 (-0.880–+1.058) | -0.114 (-0.880–+1.058) | 0.45 | 3.454 |
| step-b | Core/binding와 port | 1.913 (1.793–2.349) | +0.025 (-0.529–+0.768) | -0.049 (-0.559–+0.773) | 1.45 | 0.463 |
| step-b | 분류·queue·claim과 나머지 receive dispatch | 2.281 (2.065–2.292) | -0.023 (-0.034–+0.208) | -0.001 (-0.136–+0.341) | 0.25 | 2.031 |
| step-b | Envelope header | 0.072 (0.061–0.101) | -0.001 (-0.075–+0.013) | +0.580 (+0.425–+0.651) | 0.15 | 0.000 |
| step-b | Route·DI·invoke·terminal과 나머지 | 2.628 (2.231–2.901) | +1.276 (+0.106–+1.675) | +1.162 (+0.125–+1.834) | 0.45 | 2.178 |
| step-c | Core/binding와 port | 2.416 (1.869–2.466) | -0.504 (-0.673–+0.480) | -0.552 (-0.676–+0.698) | 1.45 | 0.966 |
| step-c | 분류·queue·claim과 나머지 receive dispatch | 2.053 (2.034–2.070) | +0.229 (-0.005–+0.258) | +0.227 (+0.086–+0.371) | 0.25 | 1.803 |
| step-c | Envelope header | 0.058 (0.046–0.077) | +0.014 (-0.016–+0.055) | +0.594 (+0.450–+0.666) | 0.15 | 0.000 |
| step-c | Route·DI·invoke·terminal과 나머지 | 2.186 (2.144–2.768) | +0.442 (-0.537–+0.757) | +1.604 (+0.258–+1.921) | 0.45 | 1.736 |
| step-d | Core/binding와 port | 1.808 (1.735–2.397) | +0.608 (-0.528–+0.731) | +0.056 (-0.607–+0.831) | 1.45 | 0.358 |
| step-d | 분류·queue·claim과 나머지 receive dispatch | 2.004 (1.883–2.108) | +0.048 (-0.073–+0.187) | +0.275 (+0.048–+0.523) | 0.25 | 1.754 |
| step-d | Envelope header | 0.050 (0.044–0.066) | +0.008 (-0.020–+0.033) | +0.602 (+0.461–+0.669) | 0.15 | 0.000 |
| step-d | Route·DI·invoke·terminal과 나머지 | 2.540 (2.008–2.637) | -0.353 (-0.493–+0.759) | +1.250 (+0.389–+2.057) | 0.45 | 2.090 |

각 run의 네 부품 합은 그 run의 profile user CPU와 같다. 부품별 중앙값의 합과 전체 중앙값은 서로 다를 수 있다.
<!-- /PARTS_TABLE -->

## 남은 예산 차이

단계 (d)의 별도 profile 중앙값은 port 1.808, queue·claim·나머지 receive dispatch 2.004, header 0.050, route·DI·invoke·terminal·나머지 2.540 µs/message다. Header를 제외한 세 부품이 예산을 초과한다. 범위가 넓은 부품은 이전 단계 대비 효과를 확정하지 않았다. Perf 없는 최종 CPU 3회와 unpaced 5회 모두 목표에 미달했다.

Port에는 native received의 parts 이전·close와 routing-id 값 구성, 기존 socket gate가 남아 있다(backend/raw_route_port.cpp:473). Queue 경로에는 list 원본 수락·이전과 host receive_record 구성, parts와 native owner 구성이 남아 있다(mesh/service_mailbox.cpp:177, stateful/public_host_runtime.cpp의 dispatch_application_claim). Route 경로에는 context·completion 값 구성과 새 scope 생성·factory 조회·정리, typed invoker 결과 처리가 남아 있다(channels/route_packet_dispatcher.cpp:256, channels/route_handler_invoker.cpp:163, configuration/services.cpp:254). 이는 profile에 남은 호출 경로이며 각 한 항목의 비용을 따로 측정한 값은 아니다.

공통 ready coroutine만 없앤 결과를 typed adapter·filter까지 모두 제거한 결과로 보지 않는다. Request-kind helper를 합친 것이 전체 route·DI 비용 제거를 뜻하지도 않는다. 목표를 낮추지 않았으며 현재 설계와 구현으로 예산에 도달했다고 판단하지 않는다.

<!-- CPU_TABLES -->
## 같은 rate CPU 전체 결과

각 값은 µs/message의 중앙값과 3회 최솟값–최댓값이다. Source는 CPU 0–7, target은 8–15다. Rate는 136,889 message/s, payload는 1 KiB다. Perf를 켠 별도 profile은 이 표에 넣지 않았다.

| 비교 | 판 | source user | source kernel | target user | target kernel |
|---|---|---:|---:|---:|---:|
| step-a | baseline | 4.225 (4.180–4.247) | 3.007 (2.999–3.008) | 8.615 (8.384–8.692) | 2.787 (2.724–2.863) |
| step-a | step-a | 4.209 (4.185–4.256) | 2.996 (2.988–3.057) | 7.770 (7.760–7.903) | 2.946 (2.875–2.984) |
| step-b | baseline | 4.222 (4.210–4.327) | 2.978 (2.948–3.047) | 8.508 (8.462–8.800) | 2.810 (2.730–2.852) |
| step-b | step-a | 4.293 (4.235–4.344) | 3.011 (3.008–3.023) | 7.785 (7.718–7.823) | 2.920 (2.845–3.047) |
| step-b | step-b | 4.079 (4.000–4.094) | 2.864 (2.818–2.912) | 6.795 (6.655–6.804) | 2.973 (2.860–3.044) |
| step-c | baseline | 4.103 (4.070–4.225) | 2.880 (2.879–3.008) | 8.303 (8.213–8.577) | 2.770 (2.748–2.809) |
| step-c | step-b | 4.105 (4.064–4.271) | 2.925 (2.878–3.022) | 6.820 (6.774–7.001) | 2.834 (2.811–3.155) |
| step-c | step-c | 4.082 (4.028–4.182) | 2.910 (2.780–2.948) | 6.595 (6.380–6.690) | 2.894 (2.786–2.934) |
| step-d | baseline | 4.194 (4.182–4.218) | 2.966 (2.938–2.967) | 8.490 (8.437–8.661) | 2.838 (2.824–2.943) |
| step-d | step-c | 3.994 (3.965–4.003) | 2.850 (2.823–2.896) | 6.501 (6.485–6.645) | 2.799 (2.786–2.820) |
| step-d | step-d | 4.138 (4.100–4.185) | 2.922 (2.916–2.927) | 6.618 (6.501–6.781) | 3.007 (2.951–3.025) |
| final | binding | 1.571 (1.519–1.599) | 1.080 (1.053–1.175) | 1.434 (1.342–1.442) | 1.032 (0.820–1.035) |
| final | main | 5.289 (5.190–6.210) | 3.788 (3.720–4.399) | 9.567 (9.461–11.180) | 3.673 (3.569–4.013) |
| final | step-d | 4.118 (4.009–4.287) | 3.000 (2.884–3.056) | 6.775 (6.724–6.912) | 2.982 (2.902–3.068) |

원본과 계산은 raw/<비교>/paced, scripts/summarize.py와 raw/cpu-summary.json에 있다.
<!-- /CPU_TABLES -->

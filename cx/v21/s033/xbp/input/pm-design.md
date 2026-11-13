# C++ Framework F÷B 0.8 설계 — 원인 분해와 계약 변경 대안

읽기 전용 설계 문서다. 코드·스펙·빌드·측정·git 변경은 없다. 모든 µs 값은 **가설**이며 계산식을 함께 적는다.

## 0. 기준과 입력

| 항목 | 값·출처 |
|---|---|
| 저장소 | `D:/worktree/zlink-fable-design`, `39bbfe470d` detached |
| kairos 설계서 기준 revision | `532cd83b67`. `git log 532cd83b67..HEAD -- framework/languages/cpp/framework`는 0건이다. 설계서의 C++ `file:line`은 이 worktree에서 그대로 유효하다. |
| 24셀 표 | `D:/t/jobs/fx/cx/jfable-08/table.md`(kairos 11-10). C++ 6칸은 ratio051pr2(11-08), .NET·Java request는 11-08, send와 Node는 ratio040(11-05)이다. 칸마다 세션이 다르다. |
| 역압 계수 | `D:/t/jobs/fx/cx/jfable-08/count-report.md`. p_s·p_r ≈ 0.33–0.37 %, request K_c ≈ 1.0, backpressure K(worker 알림당 처리) = 72.5·122.5, send K ≈ 3,316·2,795, request 즉시 완료 0. |
| 설계서 | `D:/t/jobs/fx/cx/rev1639cpath/report.md`(이하 R). §4의 S1–S6·R1–R10·Q1–Q9·C1–C5 ID와 µs 가설을 그대로 인용한다. |

경로 접두어는 R과 같다. `C`=`framework/languages/cpp/framework/src/runtime/`, `H`=`framework/languages/cpp/framework/include/zlink/framework/`, `B`=`bindings/cpp/`, `P`=`framework/bench/grpc/cpp/`, `N`=`framework/languages/dotnet/src/Zlink.Framework/Runtime/`, `J`=`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/`, `S01`·`S02`·`S04`·`S05`·`S06`·`S08`=`framework/doc/framework/common/spec/server/01-execution/0N-*.ko.md`, `W`=`02-channel-transport/06-wire-protocol.ko.md`, `API`=`00-foundation/06-framework-api.ko.md`, `CPP`=`languages/cpp/interfaces/03-channel-messaging.ko.md`, `MM`=`00-foundation/05-message-model.ko.md`.

**0.8 허용량의 정의.** 브리프대로 Framework가 더할 수 있는 메시지당 시간은 binding 시간의 25 %다: `A = 0.25 × 10^6 / B`. 포화 셀에서 처리량은 가장 바쁜 thread의 메시지당 CPU가 정하므로, 허용량은 그 thread의 binding CPU 위에 더하는 값으로 비교한다(R §1의 caller 열). binding caller가 포화가 아닌 send 1 KiB에서는 `T_0.8 − b_caller`가 실제 상한이므로 둘을 함께 적는다.

| 셀 | B msg/s | T_B µs | A = 0.25·T_B | T_0.8 = T_B/0.8 | binding caller CPU | 현재 F caller CPU | 현재 F가 더한 caller CPU |
|---|---:|---:|---:|---:|---:|---:|---:|
| send 1024 | 600,544 | 1.665 | 0.416 | 2.081 | 1.344 | 4.766 | **+3.422** |
| send 4096 | 278,282 | 3.594 | 0.898 | 4.492 | 1.938 | 5.190 | +3.252 |
| backpressure 1024 | 249,415 | 4.009 | 1.002 | 5.012 | 4.006 | 17.413 | **+13.407** |
| backpressure 4096 | 152,591 | 6.554 | 1.638 | 8.192 | 6.405 | 19.957 | +13.552 |

caller CPU 값은 R §1 둘째 표(thread user+kernel)다. process CPU의 0.8 몫(`binding_CPU × 0.25`)은 send 1024 source 0.733·target 0.694, backpressure 1024 source 2.392·target 2.344다(R §1 셋째 표에서 계산).

**절대 목표.** 0.8이면 C++ F는 send 1024 480k, backpressure 1024 200k msg/s이고 다른 언어 최고(.NET 178,561·Java 68,227)를 넘는다. 0.6에서도 넘는다(149,649 > 68,227). 절대 목표는 0.8 설계를 더 제약하지 않는다.

## 1. 원인 분해 — 1 KiB send와 1 KiB backpressure

"현재"는 R §4가 ccall2·csrc·ctrecv raw에서 재계산한 stage 값이고, "1~7단계 뒤"는 R §4 표의 가설이다. 두 열 모두 binding 위에 더하는 user+kernel CPU다. 현재 열이 비어 있는 단계는 R의 profile이 그 단계를 분리하지 못한 것이다.

### 1.1 send 1 KiB — source caller (binding caller 1.344 µs 위)

현재 caller 추가 합계는 3.422 µs다. R §4는 csrc에서 service frame 조립 0.372, header 0.089만 분리했다. 나머지 약 2.96 µs는 아래 (a) 항목들의 합으로만 설명되며 단계별 분리값이 없다.

| 단계 | 지금 하는 일 | 현재 위치 | 1~7단계 뒤(R) | 근거 조항 |
|---|---|---|---|---|
| S1 call·codec plan | public call object, protobuf plan 조회 | `C/channels/channel_runtime.cpp:1480-1485` | 0.10–0.20 | S01 §2 `:38`; S05 §7 `:189-225` |
| 식별 조회 | 매 send마다 `routing_id_async()` task를 만들고 `project_task_terminal`로 결과를 감싼다 | `C/host/app.cpp:2601-2655` | 0(bind 때 보관) | 조항 없음. (a) |
| S2 lane·admission·port gate | `_lane.run_sync` 람다 → `peer` 조회 → `submit_send` → `_socket_mutex` | `C/mesh/raw_mesh_node_owner.cpp:1656-1664,1683-1686,1759-1766`; `C/backend/raw_route_port.cpp:190-216` | 0.12–0.30 | S08 E2·E4 `:97,99`; S06 §5 `:166-175`; W §4 `:330` |
| S3 application JSON header | `encode_node_send_parts`가 header bytes를 만든다 | `C/channels/channel_runtime.cpp:1481-1484`; `C/messaging/envelope_codec.cpp:365-373` | 0.09–0.12 | W §2 `:181-184`; S08 E1 `:96` |
| S4 service payload 조립 | multipart profile(count+length)로 header·body를 outer frame 하나에 쓴다. 1 KiB body 복사 1회 | `C/protocol/service_wire_codec.cpp:3218-3230`; `C/mesh/raw_mesh_node_owner.cpp:1557` | 0.30–0.45 | W §2 `:186-213`; S08 E1 `:96` |
| S5 binding 이전·결과 변환 | parts를 operation으로 이동, `async()`, 즉시 결과를 `result_t`로 | `C/backend/raw_route_port.cpp:207-216,238-241` | 0.08–0.16 | S01 §15 `:569-580`; S05 §4 `:60-75` |
| S6 trace enable 확인 | `trace`가 있으면 문자열 생성 | `C/backend/raw_route_port.cpp:217-237` | 0.02–0.04 | 03-message-flow-tracing §5 |
| pending send(p_s≈0.33 %) | TCS + `shared_ptr<task>` + observer coroutine + logical entry | `C/backend/raw_route_port.cpp:242-247`; `C/mesh/raw_mesh_node_owner.cpp:1690-1746` | p_s×(0.80–1.56) ≈ 0.003–0.005 | S01 §10 `:399-405`; S08 E3 `:98` |
| **합계** | | **3.422(측정)** | **0.71–1.27** | |

### 1.2 send 1 KiB — target (binding target process 2.774 µs 위)

| 단계 | 지금 하는 일 | 현재 위치 | 1~7단계 뒤(R) | 근거 조항 |
|---|---|---|---|---|
| 회전 진입 | record마다 `_lifecycle_mutex`로 port 참조, `_socket_mutex`로 recv, RID `to_bytes()` vector 생성 | `C/mesh/raw_mesh_node_owner.cpp:3609-3616`; `C/backend/raw_route_port.cpp:475,496-501` | 회전당 1회로 이동 | S08 I0–I2 `:183-185` |
| R1 분류·fence | `decode_header`, **무조건 문자열 결합** `trace_mesh("receive kind=" + to_string…)`, peer·generation 확인 | `C/mesh/raw_mesh_node_owner.cpp:3645-3666` | 0.18–0.30 | W §2 `:152-160`, §4 `:330`; S08 I3 `:186` |
| R2 permit·commit | `bind_application_dispatch` 람다가 record마다 `_dispatch_gate_mutex` → permit 이동 → mailbox `try_enqueue`: mutex, `owners[std::string]` map 조회, `push_back`, ready 게시 | `C/mesh/mesh_node_host_service.cpp:3367-3395`; `C/mesh/service_mailbox.cpp:88-118` | 0.10–0.18 | S04 §3 `:116-126,226-262`; S08 I4 `:187` |
| 회전·통지(÷K) | ready owner마다 `std::function` 생성 → executor queue push → `notify_one`; worker는 `drain_application_owner` coroutine 시작 | `C/mesh/mesh_node_host_service.cpp:3354-3362,2125-2141`; `C/dispatch/offload_executor.cpp:92-107,331-345` | (0.30–0.80 + 0.80–2.00 + 0.18–0.46)/K, K≈3,316 → ≈0.001 | S08 §4.4 `:300-316`; S04 §3 ready 집합 `:189-222` |
| claim·batch | owner claim마다 `make_shared<claim>`, `records`·`record_bytes` vector, `atomic_bool[]`, record마다 settlement; batch 객체와 `take_unless` 람다, stateful 검사용 `decode_header` 재호출 | `C/mesh/service_mailbox.cpp:160-215`; `C/stateful/public_host_runtime.cpp:757-766,8121-8160,8557-8592` | 0(삭제, 2단계) | S02 §2 `:94-104`; S04 §3 `:189-222` |
| R3 head take | prefix claim 뒤 record 이동 | 위와 같음 | 0.10–0.18 | S02 §2 `:97`; S04 §3 `:194` |
| R4·R5 header | `decode_mesh_record_header_view` → JSON header 해석. 반복 header는 canonical reader hit | `C/mesh/mesh_node_host_service.cpp:3800-3803`; `C/messaging/envelope_codec.cpp:935-937,1230-1245` | 0.08–0.14 + hit 0.07–0.10 | W §2 `:181-184`; S05 §6 `:130-150` |
| dispatcher 포장 | record마다 `mesh_record_dispatcher_t`, `route_packet_dispatcher_t`, reply 람다, `route_received_packet_t`, `complete` std::function(header 캡처) | `C/mesh/mesh_node_host_service.cpp:3823-3832`; `C/mesh/mesh_record_dispatcher.cpp:32-74`; `C/channels/route_packet_dispatcher.cpp:288-310` | 0(삭제, 3단계) | 조항 없음. (a) |
| R6·R7 scope·handler | `service_scope_t::create` → `make_shared<service_scope_state_t>`; 벤치는 singleton을 돌려받는다 | `C/channels/route_handler_invoker.cpp:37-39`; `C/configuration/services.cpp:254-269,299-300`; `P/framework/bench_framework_cpp_server.cpp:59-63` | 0.08–0.16 + **0.18–0.35(#4145 뒤 추가)** | API §10 `:485-489`, §11 `:500-513`; S08 §4.3 `:253-265` |
| R8 invoker coroutine | `invoke` coroutine + promise의 `make_shared<coroutine_task_shared_state_t>` | `C/channels/route_handler_invoker.cpp:10-50`; `H/contracts/dispatch/task.hpp:1167-1179` | 0.18–0.46 | CPP §3 `:782-790`; S08 W3 `:222` |
| R9·R10 terminal·trace | permit 반환 atomic fast path, record 반환 | `C/dispatch/application_job_queue.hpp:977-995` | 0.08–0.16 + 0.02–0.04 | S04 §3 `:264-275`; API §10 `:488` |
| **합계** | | 분리 측정 없음 | **1.23–2.48** (K=8 가정; 측정 K≈3,316이면 1.05–1.72) | |

K=8 가정의 회전 분담 `(1.28–3.26)/8 = 0.16–0.41`을 빼고 측정 K로 다시 나누면 `≈0.001`이므로 target send 합계의 실질 가설은 **1.07–2.07**이다(`1.23−0.16`, `2.48−0.41`).

### 1.3 backpressure 1 KiB — source caller (binding caller 4.006 µs 위)

현재 caller 추가 13.407 µs의 분해는 R §4의 ccall2 무프로파일 투영이다: encode 2.122, register 5.755, observe 2.761, binding 5.000(binding 벤치 caller 4.006과 겹침), other 0.974.

| 단계 | 지금 하는 일 | 현재 위치 | 현재(ccall2 투영) | 1~7단계 뒤(R) | 근거 조항 |
|---|---|---|---|---|---|
| Q1 deadline·call | `request_started + timeout`, `remaining_request_timeout` | `C/channels/channel_runtime.cpp:1838-1846,1884-1887` | encode 2.122에 포함 | 0.10–0.20 | S01 §9 `:357-365` |
| Q3 동적 JSON header | `create_envelope(request, …, remaining timeout, correlation)` → JSON 문자열(correlationId·deadline) | `C/channels/channel_runtime.cpp:1847-1850`; `C/messaging/envelope_codec.cpp:365,452` | 〃 | 0.35–0.60 | W §2 `:181-184`; S01 §9 `:357` |
| Q4 service payload 조립 | `encode_request` → outer frame 하나에 header·body 복사 | `C/channels/channel_runtime.cpp:1872-1876`; `C/mesh/raw_mesh_node_owner.cpp:1557` | 〃 | 0.30–0.45 | W §2 `:186-213` |
| Q6 receiver 저장소 | `make_shared<receiver_state_t>`(admitted TCS + reply stage + physical stage + project), projection capture 할당 | `H/contracts/dispatch/request_task.hpp:648-712,714-740,145-149`; `C/channels/channel_runtime.cpp:1880-1896` | register 5.755에 포함 | 0.20–0.35 | S01 §9 `:331-333`, §2 `:68` |
| Q2 lane turn | `_lane.run_sync` 람다(shared_ptr 다수 캡처) → peer 조회 → port | `C/mesh/raw_mesh_node_owner.cpp:1551-1565` | 〃 | 0.12–0.30 | S08 E2–E4 `:97-99`; S06 §5 |
| Q5 logical entry | `register_operation`: `pending_t`(`std::vector<uint8_t> target_routing_id` 포함), C1 lock, deadline index link, stop_token 연결 | `C/foundation/operation_registry.cpp:109-119,382-394,488-495` | 〃 | 0.30–0.55 | S01 §10 `:424-429`, §10.1 `:447-456`; S08 E3 `:98` |
| Q7 observer coroutine | `observe_request_completion`: admission await, `admitted->complete`, reply await, `unregister`, `completion(...)` | `C/backend/raw_binding_adapter.hpp:146-199` | observe 2.761 | 0.18–0.46 | S08 E4·E5 `:99-100`; CPP §3 `:788` |
| Q8·Q9 | 값 변환·trace 확인 | `C/channels/channel_runtime.cpp:1765-1836` | other 0.974 일부 | 0.08–0.16 + 0.02–0.04 | S05 §6 `:135` |
| 벤치 측 observe | 요청마다 `observe_task_completion` 2회(admitted, 완료) → observer `make_shared` 2개 | `P/client/bench_cpp_client.cpp:939-947`; `H/contracts/dispatch/task.hpp:767-770` | other 0.974 일부 | 공개 API 비용. 4단계가 block을 줄임 | 조항 없음 |
| **합계** | | **13.407(측정)** | **1.65–3.11** | |

### 1.4 backpressure 1 KiB — completion(source dispatcher·poller)과 target

| 단계 | 지금 하는 일 | 현재 위치 | 1~7단계 뒤(R) | 근거 조항 |
|---|---|---|---|---|
| C1 reply header decode | reply마다 `envelope.decode_header`(correlationId 포함 → cache hit 불가) + body view | `C/channels/channel_runtime.cpp:1781-1800` | 0.65–1.00 + 0.08–0.14 | W §2 `:152-160`; S05 §6 `:135` |
| C3 terminal take | `unregister(id)` → C1 lock, `take_completion`, cancellation reset | `C/foundation/operation_registry.cpp:128-135,545-550` | 0.10–0.20 | S01 §10 `:392-397` |
| C4 publish·정리 | projection → `physical.complete` → `reply.complete` | `H/contracts/dispatch/request_task.hpp:690-706` | 0.10–0.20 | S01 §9 `:333` |
| C5 새 completion turn | `post_completions`가 완료 **하나마다** `dispatch_completion` item을 게시; K_c≈1.0 측정 | `C/foundation/operation_registry.cpp:269-290`; count-report K_c | 0.10–0.20 + **(0.80–2.00)/K_c = 0.80–2.00** | S01 §11 `:496-513`; S08 §4.4 `:306-308` |
| **source completion 합계** | | | **1.83–3.74** | |
| target request | R1–R10(§1.2)에서 R5가 miss(0.65–1.00)로 바뀜 + typed adapter coroutine 0.18–0.46 + reply JSON header 0.35–0.60 + reply service 조립 0.30–0.45 + `_socket_mutex` 아래 동기 `reply().submit()` | `H/contracts/configuration/mesh_node.hpp:640-700`; `C/backend/raw_route_port.cpp:509-535`; `C/mesh/raw_mesh_node_owner.cpp:3636-3641` | **2.48–4.48 + (1.28–3.26)/K**, K≈72 → 2.50–4.53 | S08 W4 `:223`; W §2 `:186-213`; API §10·11 |

### 1.5 Framework 바깥의 미확정 항 — Core I/O thread(Δ_native)

R §1은 ccall2에서 target I/O1이 0.813 core였다고 적는다(`R:48`). 같은 ccall2의 caller는 19.078 µs/request에 0.824 core이므로 처리율은 `0.824 / 19.078e-6 ≈ 43,200 msg/s`이고, I/O1의 메시지당 CPU는 `0.813 / 43,200 ≈ 18.8 µs`다. binding 벤치는 249,415 msg/s를 I/O thread 1 core 이하로 처리하므로 I/O thread는 메시지당 `≤ 4.01 µs`다. **가설: Framework 경로에서 Core I/O thread의 메시지당 CPU가 binding 경로의 4배 이상이다.** 원인 후보는 frame 수·byte 수 증가(frame 0 + JSON header + body, reply도 같다), worker thread에서 reply마다 동기 submit(`C/backend/raw_route_port.cpp:509-535`)이 I/O thread를 깨우는 횟수, Completion connection 경로다. 이 값이 사실이면 Framework 쪽 µs를 모두 줄여도 target I/O thread가 `10^6 / 18.8 ≈ 53k msg/s`에서 처리량을 막는다 — 현재 F 51,549와 거의 같다. **0.8 설계의 첫 측정 항목이다**(§6).

## 2. 단계 분류 — (a) C++만 지불 / (b) 계약 요구 / (c) 미확정

| 단계 | 분류 | 근거 — 다른 언어 대조 또는 소유 조항 |
|---|---|---|
| 식별 조회 task(send) | (a) | .NET `N/Backend/DotNet/Wrappers/ZLinkBackendSpotNodeWrapper.cs:1312-1318`는 `SendToNodeDirectAsync`를 바로 부른다. |
| S2·Q2 lane turn | (b) | S08 `:102-113` 필수 소유 turn. .NET `N/Execution/ZLinkStateLane.cs:97-120`은 무경합이면 TCS 없이 caller가 turn을 실행한다 — C++의 람다 캡처 비용은 (a). |
| S3 header bytes | (b)+(a) | W §2 `:181-184`. Java `J/messaging/ZLinkChannelEnvelope.java:510-530`은 단순 header를 cache한다. C++ 6단계가 같은 cache를 둔다. |
| S4·Q4 outer 조립, 4 KiB +0.192 | **(b)** | W §2 `:186-213` multipart profile. 네 언어 모두 지불한다. |
| Q3 동적 JSON header, C1·R5(request) header 해석, reply header encode | **(b)** | W §2 `:181-184`; MM `:75`(CorrelationId). frame 0은 이미 binary correlation을 싣는다(`C/protocol/service_wire_codec.cpp:553-568`). 같은 사실이 wire에 두 번 있다. .NET `N/Messaging/ZLinkEnvelopeCodec.cs:199,211,399`도 correlated header를 cache하지 못한다(R Q3·C1). |
| Q5 logical entry·deadline index | **(b)** | S01 §10 `:424-429`, §10.1 `:447-456`. Java `J/internal/service/ZLinkServiceOperationRegistry.java:216-220,328-350,511-519`도 owner turn에서 등록·take한다. binding은 자기 completion entry를 따로 가진다(`B/src/Runtime/Messaging/completion_owner.cpp:722-730`). |
| Q6 call·receiver 저장소 | (b)/(c) | S01 §9 `:331-333` 최종 수신자. 블록 수(현재 3+)는 (a). |
| Q7 observer coroutine | (b) | S08 E4·E5; CPP §3 `:782-790`. 제거는 금지 패턴. |
| C5 새 completion turn, (0.8–2.0)/K_c | **(b)** | S01 §11 `:496-513`; S08 §4.4 `:306-308`은 binding completion poller를 수신 역할로 분류한다. .NET `N/Backend/DotNet/ZLinkCompletionDispatcher.cs:13-17`(ThreadPool post), Java `J/internal/service/ZLinkServiceCompletionDispatcher.java:51-53`(commonPool execute)도 완료마다 게시한다. binding 벤치는 poller thread에서 바로 재개한다(`P/client/bench_cpp_client.cpp:628-655`; `B/src/Runtime/Messaging/async_operation_state.hpp:120-134`). |
| 회전 진입 mutex 2개, RID vector, trace 문자열 | (a) | .NET `N/Service/ZLinkManagedMeshNode.cs:6332-6370`은 record마다 lock 없이 `Recv`·`Claim`·`ProcessReceived`를 잇는다. .NET `N/Handlers/ZLinkHandlerDispatcher.cs:105-107`은 `Flow.Enabled` 안에서만 trace 객체를 만든다. |
| R2 permit·commit | (b) | S04 §3 `:116-126,226-262`. owner key가 `std::string`인 것은 (a). |
| claim·batch·settlement, dispatcher 포장, 완료 std::function | (a) | .NET `N/Spots/ZLinkMeshNodeRouteDispatcher.cs:220-259`는 batch list를 순회하고 `:692-700`은 static 람다에 state tuple을 넘긴다. R 2·3단계가 삭제 대상이다. |
| R3 head take | (b) | S02 §2 `:97-104`; S04 §3 `:194`. |
| R6·R7 scope·handler 새 생성 | **(b)** | API §10 `:485-489`, §11 `:500-513`; S08 §4.3 `:253-257`. .NET `N/Handlers/ZLinkHandlerDispatcher.cs:96-102`, Java `J/channels/ZLinkChannelHandlerInvoker.java:443-452`가 dispatch마다 만든다. C++ 벤치는 singleton(#4145, `services.cpp:299-300`)이라 지금 비용이 빠져 있다. |
| R8 invoker coroutine, typed adapter coroutine | (b) | CPP §3 `:782-790`. shared state `make_shared`는 (a), 4단계. |
| 회전·통지 ÷K | (b) | S08 §4.4; S04 §3 ready 집합. 측정 K가 커서 send·backpressure에서는 무시 가능, serial(K=1)에서만 크다. |
| Δ_native(I/O thread) | **(c)** | 같은 세션 thread CPU 미제공. §1.5. |
| 벤치 측 observe 2회·caller에서의 reply 검증 | (c) | binding 벤치는 poller에서 `record_reply`(`P/client/bench_cpp_client.cpp:647-653`), Framework 벤치는 caller loop에서 `decode_payload`(`:950-960`). caller CPU 비교에 들어 있다. |

## 3. 0.8 허용량과 남는 차이

가설 합계는 §1의 값이다. 1~7단계 뒤의 회전 분담은 측정 K로 다시 나눈 값을 쓴다.

| 셀·thread | 허용량 A(25 %) | 보조 상한 T_0.8−b | 현재 추가 | 1~7단계 뒤 | 남는 차이(1~7단계 뒤 − A) |
|---|---:|---:|---:|---:|---:|
| send 1024 source caller | 0.416 | 0.737 | 3.422 | 0.71–1.27 | **+0.29–0.85** (보조 상한 기준 −0.03–+0.53) |
| send 1024 target process | 0.694 | — | 미측정 | 1.07–2.07 | +0.38–1.38 (thread별로는 ingress ≈0.48+binding recv, worker 나머지) |
| backpressure 1024 source caller | 1.002 | 1.006 | 13.407 | 1.65–3.11 | **+0.65–2.11** |
| backpressure 1024 source process(caller+completion) | 2.392 | — | 미측정 | 3.48–6.85 | +1.09–4.46 |
| backpressure 1024 target process | 2.344 | — | 미측정 | 2.50–4.53 | +0.16–2.19 |
| backpressure 1024 target I/O thread(Δ_native) | 5.012(간격) | — | ≈18.8(가설) | 변화 없음 | **+13.8** — 사실이면 다른 모든 항을 압도한다 |

결론: 1~7단계만으로는 어느 1 KiB 셀도 0.8의 허용량 안에 들지 않는다. 남는 차이의 주된 (b) 항목은 send에서 S4(0.30–0.45), backpressure에서 Q3+Q4(0.65–1.05)·Q5(0.30–0.55)·C5 통지(0.80–2.00)·target의 header 해석과 reply header·조립(1.30–2.05)이다.

## 4. 계약·구조 변경 대안

세 방향은 서로 다른 축이다 — A는 **wire 표현**, B는 **완료 turn의 실행 자원**, C는 **pending authority의 소유**. 금지 패턴(timeout·retry 증가, 두 번째 poller, 같은 사실 두 곳 보관, coroutine 제거, callback drain 합치기)은 포함하지 않는다. R §6b의 csendb·csasm(C++ 전용 source 조립 변경)과 A의 차이는 A가 wire 자체를 바꿔 **조립 단계를 네 언어에서 없애는** 것이고, 같은 wire 위에서 encoder만 바꾸는 것이 아니라는 점이다.

### 대안 A — 동적 사실은 frame 0에만, application parts는 native multipart로

**바뀌는 계약.**

| 조항 | 현재 | 제안 문장 |
|---|---|---|
| W §2 Framework multipart application profile `:186-213` | outer envelope 하나에 `u32 count + (u32 len + bytes)*` | "application message의 part들은 frame 1(metadata) 다음에 **각각 하나의 native frame**으로 싣는다. count와 length는 transport multipart가 제공하며 Framework는 outer envelope를 만들지 않는다." |
| W §2 Typed payload envelope `:181-184`; MM §5 `:75` | JSON header가 `correlationId`·`deadline`을 문자열로 싣는다 | "correlation과 deadline은 frame 0 command body(`channelRequest`·`nodeRequest`·`reply`)의 binary field가 유일한 소유자다. application JSON header는 kind·channelName·messageName·contentType·metadata·flow만 가지며 같은 등록에서 byte가 같다." |
| schema `framework/runtime/protocol/service-wire-v1.schema.json:930-933` | `channelRequest` body = correlation u64 + channel name(`C/protocol/service_wire_codec.cpp:553-568`) | body에 `deadlineMillis u32`(남은 시간) 추가. reply(command 20) body는 이미 correlation·terminal result·error code를 가진다(`C/mesh/raw_mesh_node_owner.cpp:3636-3641`). |
| S08 E1 `:96` | "하나의 frame으로 합쳐 보내는 service wire라면 … buffer 하나" | "part 목록을 그대로 보내는 형식이다. Framework가 추가하는 복사는 0이고 wire 필수 쓰기도 0이다." |
| S01 §10 `:428-429` | "Wire request는 두 값을 각각의 field로 보존" | 유지. 변경 없음. |

**네 언어 wire·구현 영향.** frame 수가 2(+metadata)에서 3(+metadata)으로 늘고 byte 수는 outer header(count·length 8–12 byte)만큼 준다. 네 언어의 service codec·envelope codec·schema validator·cross-language contract test가 함께 바뀐다. 송신: Q3가 정적 bytes 재사용(0.02–0.05)으로, Q4·S4가 0으로 바뀐다. 수신: R5·C1이 byte 일치 hit(0.07–0.10)가 되고(error reply만 miss), target reply header encode도 정적 bytes가 되며 reply 조립이 0이 된다. 4 KiB는 service payload 쓰기마다 0.192 µs가 추가로 사라진다.

**판정 위치(규칙 수).** correlation·deadline 사실의 wire 소유 위치 **2→1**(frame 0 binary + JSON header → frame 0). payload framing 결정 **2→1**(service multipart profile 조립 + native multipart → native multipart). JSON header의 동적/정적 분기(`header_plan_t::matches`, `C/messaging/envelope_codec.cpp:378-385`) **2→1**(정적만). 합계 **6→3**. 새로 생기는 판정 위치: 없음(deadline field 읽기는 기존 frame 0 decode 자리).

**절감 가설(µs/message).**

| 셀 | 항목 | 계산 | 절감 |
|---|---|---|---|
| send 1024 source | S4 0.30–0.45 + S3 (0.09–0.12 → 0.02–0.05) | `0.30+0.07`, `0.45+0.07` | **0.37–0.52** → 1~7 뒤 0.71–1.27이 **0.34–0.75** |
| send 1024 target | R4 part 재포장 일부 0.04–0.06 | | 0.04–0.06 |
| backpressure 1024 source caller | Q3 (0.35–0.60 → 0.02–0.05) + Q4 0.30–0.45 | `0.33+0.30`, `0.55+0.45` | **0.63–1.00** → 1.65–3.11이 **1.02–2.11** |
| backpressure 1024 source completion | C1 header (0.65–1.00 → 0.07–0.10) | | **0.58–0.90** |
| backpressure 1024 target | R5 miss→hit 0.58–0.90 + reply header 0.33–0.55 + reply 조립 0.30–0.45 | | **1.21–1.90** → 2.50–4.53이 **1.29–2.63** |
| 4 KiB 추가 | 0.192 × 쓰기 수(send 1, request 2) | | send 0.19, request 0.38 |

**잃는 보장.** 없음에 가깝다. metadata frame 규칙(W §2 `:143-150`)과 decode 상한(`:163-179`)은 유지한다. part 수 상한을 wire가 새로 정해야 한다(현재 outer envelope의 u32 count가 상한을 암시). Δ_native 위험: frame이 하나 늘어 Core의 frame당 비용이 는다 — §1.5 측정으로 먼저 확인한다.

**위험.** 네 언어 동시 변경과 wire 호환성(wire major 변경). csendb·csasm와 달리 encoder 대체가 아니라 조립 단계의 삭제이므로 "deferred encoder 비용 상쇄"(R §6b)가 생기지 않지만, 이는 count test(outer buffer 생성 0, 메시지당 native frame 수 = parts+1(+metadata))로 확인해야 한다.

### 대안 B — 완료 turn의 실행 자원: binding completion 자원을 수신 역할에서 분리

**B1 (스펙 변경, 네 언어).**

| 조항 | 현재 | 제안 문장 |
|---|---|---|
| S08 §4.4 `:306-308` | "E5의 완료 알림을 받는 binding poller 구동도 여기(수신 역할)에 든다" | "binding이 자기 completion drain의 독립 진행(HWM 재시도·entry 정리·종료)을 소유하는 경우(S01 §15), 그 완료 알림 자원은 수신 역할이 아니다. E5의 caller continuation은 S01 §11의 새 turn 조건(완료 판정 잠금과 lane-current scope 반환 뒤)을 지켜 그 자원에서 바로 실행할 수 있다." |
| S01 §11 `:496-499` | "잠금을 놓은 직후 같은 호출 stack에서 callback을 바로 실행하는 것만으로는 충분하지 않다 … completion dispatcher에 넣고" | "완료 알림 자원이 수신 역할(S08 §4.4)을 맡은 경우에만 dispatcher에 넣는다. 그 밖에는 잠금과 lane-current scope를 반환한 뒤 같은 자원의 다음 turn에서 실행한다." |

**영향.** C++ `post_completions`(`C/foundation/operation_registry.cpp:269-290`)가 item 게시 대신 잠금 밖에서 callback을 호출한다. .NET `ZLinkCompletionDispatcher.Post`(`N/Backend/DotNet/ZLinkCompletionDispatcher.cs:13-17`)와 Java `completions.post`(`J/internal/service/ZLinkServiceCompletionDispatcher.java:51-53`)도 같은 조건에서 inline으로 바뀐다. Node는 event loop 하나라 변화 없다.

**판정 위치.** 완료 turn의 실행 자원 결정 **2→1**(S01 §11의 dispatcher 게시 + S08 §4.4의 역할 분류 → binding drain 경계 하나). 완료당 dispatcher item **1→0**.

**절감 가설.** C5 item 0.10–0.20 + 통지 (0.80–2.00)/K_c, K_c≈1.0 → **0.90–2.20 µs/request**(source process). serial(K=1)에도 같은 값이 왕복 지연에서 빠진다. caller thread에는 직접 영향이 없다.

**잃는 보장.** S02 §13 `:500`("application handler가 대기 중이어도 infrastructure task를 진행")의 한 사례가 약해진다 — caller continuation이 binding poller thread에서 block하면 그 socket의 completion drain·재시도가 그동안 멈춘다. binding 벤치가 지금 그 조건에서 동작하고(`P/client/bench_cpp_client.cpp:628-655`), poller thread에서 다음 request를 제출하는 재진입을 binding이 허용한다는 증거다. 재진입 규칙(S06 §5 비재진입)은 lane-current scope를 반환한 뒤 실행하므로 유지된다.

**B2 (binding 계약 추가, 독립 진행 유지).** binding completion drain이 batch 경계(drain 시작·끝)를 Framework에 알리면, Framework는 drain당 한 번만 dispatcher를 깨운다(item은 완료마다 유지). 절감은 wake 분담 `(0.80–2.00) × (1 − 1/K_c')`뿐이며 K_c'(poller 기상당 완료 수)는 미측정이다. `bindings/doc/spec/` 변경이 필요하다(보호 경로). 독립 진행 보장은 그대로다. B1이 거부되면 B2를 쓴다.

### 대안 C — pending authority 하나: logical entry = binding completion entry

**바뀌는 계약.**

| 조항 | 현재 | 제안 문장 |
|---|---|---|
| S01 §10 `:392-397`, §10.1 `:447-456` | Framework가 terminal 권한 표(C1)와 deadline 인덱스(C2)를 소유하고 binding은 별도 completion entry·timeout을 가진다 | "Framework request의 pending entry는 **binding completion entry**다. Framework는 submit 때 `OperationId`·`ReplyRouteId`·남은 deadline·최종 수신자 callback을 binding operation에 등록하고, terminal 권한의 원자적 꺼내기와 deadline 만료는 binding completion drain이 확정한다. Framework는 두 번째 pending 표를 두지 않는다." |
| S08 E3 `:98` | request는 항상 pending entry·dispatcher 자리를 Framework에 등록 | "E3는 E4의 binding operation 등록 자체다. 별도 Framework 등록은 없다." |
| binding 계약(`bindings/doc/spec/`, 보호 경로) | request entry는 native correlation으로만 완료된다 | "외부 key(`OperationId`)로 entry를 완료·실패시키는 API"와 "shutdown 때 모든 entry를 terminal로 끝내는 순서"를 명시한다. |

**영향.** C++ `register_operation`·deadline index·`unregister`(`C/foundation/operation_registry.cpp:382-394,488-495,545-550`)가 request 경로에서 사라진다. Java `ZLinkServiceOperationRegistry`(`J/…:328-350,511-519`), .NET `ZLinkMeshCompletionTable`·`ZLinkRequestDeadlineIndex`(`N/Backend/DotNet/ZLinkMeshCompletionTable.cs:195-241`; `N/Messaging/ZLinkRequestDeadlineIndex.cs:215`)도 같은 방향으로 바뀐다. relocation 뒤 `replyRelay`(command 33, W §3 `:266`)는 수신 경로로 도착하므로 Framework가 `OperationId`로 binding entry를 완료시키는 외부 key API가 필요하다.

**판정 위치.** pending authority **2→1**(Framework C1 표 + binding entry → binding entry). deadline 만료 판정 **2→1**(Framework C2 인덱스 + binding timeout → binding timeout). 새로 생기는 위치: binding의 외부 key 완료 1(기존 entry take를 재사용하므로 판정 위치는 늘지 않는다).

**절감 가설.** Q5 0.30–0.55 − binding 등록 인자 추가 0.05–0.10 = **0.25–0.45**(caller), C3 0.10–0.20 → binding take로 흡수 **0.10–0.20**(completion). 합계 0.35–0.65 µs/request.

**잃는 보장.** resolve 이전 deadline은 지금도 submit 전에 `remaining_request_timeout`으로 차감되고 0 이하면 `deadline_exceeded`로 끝나므로(`C/channels/channel_runtime.cpp:1884-1887`) 그대로다. 잃는 것은 (1) Framework shutdown 순서에서 binding보다 먼저 pending을 terminal로 끝내는 능력(S01 §11 `:539-541` "shutdown에서 이미 수락한 callback을 모두 실행" — binding close 순서에 의존하게 된다), (2) binding이 폐기한 late native reply에 대한 Framework의 관측(S01 §9 `:338-340`은 이미 binding 계약에 맡긴다), (3) 10.1의 "1 ms slot 256개" 인덱스 정책 — binding timer 정책으로 대체된다.

**위험.** 가장 크다. 보호 경로인 binding 스펙과 네 언어 binding 구현이 바뀐다. relocation reply relay의 dedup(`OperationId`, W §11 `:760-770`)을 binding entry에 매달면 binding이 Framework identity를 알게 된다 — 계층 경계(`00-foundation/08-layering`)와의 충돌을 감독자가 먼저 판정해야 한다.

### 대안이 아닌 것

- **handler·scope 재사용(API §10·§11)**: "scoped dependency가 없는 handler만 재사용"은 "이 경우만" 분기를 하나 더하므로 제안하지 않는다. R7 비용은 (a) 구현(빈 저장소 재사용, `make_shared` 제거)으로만 줄인다. #4145를 고친 뒤 C++ target에 0.18–0.35가 **추가**된다는 사실은 §3 표의 target 값에 이미 넣었다.
- **owner queue·permit 제거(S04 §3)**: Core byte HWM은 같은 보호가 아니다(R §6). 제안하지 않는다.
- **receive thread에서 handler 실행(S02 §13)**: 측정 K가 72–3,316이라 send·backpressure에서 회전 분담은 0.001–0.05 µs다. 얻을 것이 없고 보장을 잃는다. serial에는 §4.4의 역할 인계가 이미 허용한 경로다.

## 5. .NET·Java가 0.8에 가까운 셀의 대조

표의 간격은 `10^6/F − 10^6/B`로 구한 **처리량 간격의 차이**이며 thread CPU가 아니다.

| 셀 | 언어 | F÷B | F가 더한 간격 µs | 25 % 허용량 µs | C++ 같은 셀: 더한 간격 / 허용량 |
|---|---|---:|---:|---:|---|
| send 1024 | .NET | 0.774 | `5.600−4.335 = 1.265` | 1.084 | `5.499−1.665 = 3.834` / 0.416 |
| send 4096 | .NET | 0.837 | `7.658−6.410 = 1.248` | 1.602 | `6.338−3.593 = 2.745` / 0.898 |
| backpressure 4096 | Java | 0.682 | `16.251−11.086 = 5.165` | 2.772 | `22.136−6.553 = 15.583` / 1.638 |
| backpressure 1024 | Java | 0.535 | `14.657−7.844 = 6.813` | 1.961 | `19.399−4.009 = 15.390` / 1.002 |

**binding 속도 차이의 몫.** C++ binding은 .NET의 2.6배(send 1024), Java의 2.0배(backpressure 1024) 빠르다. 같은 절대 비용을 더해도 C++의 비율은 그만큼 낮다. .NET send 1024에서 C++ Framework의 **절대 처리량은 .NET Framework와 같다**(181,862 vs 178,561) — C++ Framework가 메시지당 약 5.5 µs를 쓰는 것은 .NET Framework와 같은 수준이고, 언어 속도가 아니라 구조(할당·lock·handoff 수)가 정한 값이라는 뜻이다.

**구조 차이의 몫.** 더한 간격 자체도 C++이 .NET의 3.0배(send 1024), Java의 2.3배(backpressure 1024)다. 같은 계약 항목을 두 언어가 어떻게 처리하는지:

| 계약 항목 | .NET(send 0.77–0.84) | Java(backpressure 0.54–0.68) | C++ 현재 |
|---|---|---|---|
| S08 E2–E4 lane turn | `N/Execution/ZLinkStateLane.cs:97-120`: 무경합이면 caller가 할당 없이 turn 실행 | `J/internal/service/ZLinkServiceOperationRegistry.java:328-350`: `submitOwnerTurn` 람다 | `_lane.run_sync` + shared_ptr 캡처 람다(`C/mesh/raw_mesh_node_owner.cpp:1551-1565`) |
| S08 E1·W §2 header | `N/Messaging/ZLinkEnvelopeCodec.cs:188,194` 단순 header bytes 재사용(R S3) | `J/messaging/ZLinkChannelEnvelope.java:510-530` SIMPLE_HEADER_CACHE | 6단계 전. send header 매번 encode |
| S08 I4→W1 handoff | `N/Spots/ZLinkMeshNodeRouteDispatcher.cs:212-213,220-259`: receive loop의 claim turn에서 `DispatchBatchAsync`를 호출(#3995, receive→handler thread 2→1) | `J/channels/ZLinkMeshApplicationDispatcher.java:491-500` record마다 `ZLinkHandlerDispatch.enqueue`(future 생성, `J/channels/ZLinkHandlerDispatch.java:16-34`) | owner 알림마다 std::function + drain coroutine, claim·batch·dispatcher 객체(§1.2) |
| API §10·11 handler | `N/Handlers/ZLinkHandlerDispatcher.cs:96-102` scope + `catalog.Create` 매 dispatch | `J/channels/ZLinkChannelHandlerInvoker.java:443-452` 매 dispatch | singleton(#4145) — **지금 C++이 더 싸다** |
| S01 §11 completion turn | `N/Backend/DotNet/ZLinkCompletionDispatcher.cs:13-17` 완료마다 ThreadPool post | `J/internal/service/ZLinkServiceCompletionDispatcher.java:51-53` 완료마다 commonPool | 완료마다 dispatcher item(§1.4 C5) — **셋이 같다** |
| S01 §10 logical entry | `N/Backend/DotNet/ZLinkMeshCompletionTable.cs:195-241` | `J/…ZLinkServiceOperationRegistry.java:511-519` | `C/foundation/operation_registry.cpp:382-394` — **셋이 같다** |

따라서 .NET send가 0.8에 가까운 것은 (1) binding이 느려 허용량이 2.6배 크고, (2) send 경로에 handoff가 없으며(receive loop inline dispatch), (3) header cache가 있기 때문이다. (2)는 .NET이 S08 §4.4의 역할 인계를 실제로 하는지 확인하지 못했다 — `N/Service/ZLinkManagedMeshNode.cs:6191-6240`의 receive loop와 `:6332-6370`의 inline `ProcessReceived`만 확인했다(c). (b) 항목인 completion turn과 logical entry는 세 언어가 같은 구조로 지불한다 — 이 두 항목이 request 셀에서 어느 언어도 0.7을 넘지 못하는 공통 원인이다(.NET 0.62, Java 0.68, Node 0.70).

## 6. 추천 — 최소 계약 변경 묶음과 순서

### 6.1 먼저 측정으로 확정할 값(계약 변경 전, 같은 세션, 1~7단계 전·후 각 1회)

| 값 | 왜 먼저인가 | 측정 방법 |
|---|---|---|
| **Δ_native: Core I/O thread(source I/O0·target I/O1)의 메시지당 CPU, F vs B** | §1.5 가설이 사실이면 Framework 쪽 절감과 무관하게 target I/O thread가 처리량을 막는다 | thread별 schedstat(ctrecv 방식), 같은 rate로 paced. 메시지당 native frame 수·byte 수를 함께 센다 |
| K_c'(poller 기상당 Framework 완료 수)와 dispatcher wake 수 | B1·B2의 절감 크기를 정한다. count-report는 item 수(K_c≈1)만 셌다 | `offload_executor.cpp:108,334` 식 계수를 completion dispatcher에 추가 |
| caller thread의 단계별 CPU(encode·register·observe·binding·벤치 observe) | §1.3의 "현재" 열은 ccall2 투영이다. 1~7단계 뒤 재측정으로 A·C의 남은 차이를 확정한다 | perf 또는 R §4의 attribution 방식 |
| #4145 수정 뒤 R7 실측 | target 값이 0.18–0.35 오른다 | count test + 같은 세션 |

### 6.2 순서

1. **1~7단계(승인됨)를 끝내고 §6.1을 측정한다.** 계약 변경 없이 도달하는 값을 확정한다. 가설대로면 send 1024 caller 0.71–1.27(0.8 허용량 0.416 초과), backpressure 1024 caller 1.65–3.11(1.002 초과)이다.
2. **대안 A(wire).** (b) 비용 중 가장 크고 보장을 잃지 않으며 규칙 수가 6→3으로 준다. 네 언어 동시, wire major 변경. 예상: send 1024 caller 0.34–0.75(허용량 0.416 / 보조 상한 0.737 안쪽 또는 경계), backpressure 1024 caller 1.02–2.11(여전히 초과), target request 1.29–2.63(허용 2.344 경계).
3. **대안 B1(완료 turn).** source process에서 0.90–2.20 µs/request, serial 왕복 지연에서 같은 값. S02 §13의 한 사례를 약화하므로 대표님 결정 대상. 거부되면 B2(binding batch 경계).
4. **대안 C(authority 통합)**는 A·B 뒤 backpressure caller가 허용량을 넘을 때만. 예상 A+C 뒤 caller 0.77–1.66 — 하한만 1.002 안쪽이다. **A+C로도 상한에서는 backpressure 1024의 0.8이 보장되지 않는다.** 그때 남는 (b) 항목은 Q2 lane 0.12–0.30, Q6 저장소 0.20–0.35, Q7 observer coroutine 0.18–0.46, Q1 0.10–0.20이며 이 넷은 C++ interface 계약(CPP §3)과 S08 §3의 소유 turn이다.

### 6.3 1~7단계와의 관계

A는 6단계(header·codec plan 재사용)의 cache hit 조건을 request·reply까지 넓히고, 1·3단계의 canonical record에서 part 재포장을 없앤다. B1은 7단계(batch publication·wake 예약)의 completion 쪽을 대체한다 — 7단계가 dispatcher wake를 합치는 구현이면 B1 뒤에는 필요 없다. C는 5단계(최종 receiver 연결)를 그대로 쓰되 등록 대상이 binding으로 바뀐다. 어느 대안도 2·4단계(claim·batch 삭제, coroutine frame 통합)를 바꾸지 않는다.

### 6.4 처리량 환산(가설, caller가 병목일 때)

| 셀 | 조건 | caller 합계 µs | 환산 F msg/s | F÷B |
|---|---|---:|---:|---:|
| send 1024 | 1~7 뒤 | 1.344 + 0.71–1.27 = 2.05–2.61 | 383k–488k | 0.64–0.81 |
| send 1024 | +A | 1.344 + 0.34–0.75 = 1.68–2.09 | 478k–595k | 0.80–0.99(I/O thread 1.63 µs/msg가 613k에서 상한) |
| backpressure 1024 | 1~7 뒤 | 4.006 + 1.65–3.11 = 5.66–7.12 | 140k–177k | 0.56–0.71 |
| backpressure 1024 | +A | 4.006 + 1.02–2.11 = 5.03–6.12 | 163k–199k | 0.66–0.80 |
| backpressure 1024 | +A+C | 4.006 + 0.77–1.66 = 4.78–5.67 | 176k–209k | 0.71–0.84 |

이 환산은 Δ_native가 0이고 target이 병목이 아니라는 가정 위에 있다. §1.5의 가설이 사실이면 target I/O thread가 먼저 막는다.

## 7. 감독자가 결정할 것

| 번호 | 선택지 | 추천 |
|---|---|---|
| D1 | Δ_native·K_c'·단계별 caller CPU를 1~7단계 전에 측정할지, 뒤에만 측정할지 | **전·후 각 1회**. 전 측정이 없으면 1~7단계의 절감을 분리할 수 없고, I/O thread 가설을 지금 확인해야 A의 frame 수 증가 위험을 판단할 수 있다 |
| D2 | 대안 A(wire major 변경, 네 언어) 착수 여부와 시점 | **착수**. (b) 비용 중 최대이고 규칙이 줄며 보장 손실이 없다. 1~7단계 측정 뒤가 아니라 설계는 지금, 구현은 Core 1.24.0 gate 뒤 |
| D3 | B1(S08 §4.4·S01 §11 개정, 독립 진행 일부 약화) vs B2(binding batch 경계 계약 추가) | **B1**. 규칙 2→1이고 binding 벤치가 같은 조건에서 동작한다. S02 §13 약화를 받아들일 수 없으면 B2 |
| D4 | 대안 C(binding 스펙 변경, authority 통합) 검토 착수 | **보류**. A+B 측정 뒤 backpressure caller가 1.0 µs를 넘을 때만 |
| D5 | #4145(handler singleton) 수정을 측정 전에 할지 | **전에**. 지금 C++ target 값은 계약보다 0.18–0.35 싸게 측정된다 |
| D6 | 판정 기준: 이번 릴리스 0.6, 제품 0.8, S08 §7(c)의 0.90 | R §6의 미결 사항 그대로. 0.90은 이 설계에서도 근거가 없다 |

---

**요약(5줄)**
1. 1~7단계 뒤에도 C++ 1 KiB 두 셀은 0.8 허용량(send caller 0.416, backpressure caller 1.002 µs)을 가설 상한에서 0.85·2.11 µs 넘고, 남는 비용의 대부분은 스펙 계약(b)이다.
2. 가장 큰 (b) 비용은 wire다 — JSON header의 correlation·deadline은 frame 0에 이미 있는 사실의 중복이고 multipart profile은 body 복사 1회를 요구한다. 대안 A(frame 0만 동적, parts를 native frame으로)가 request 왕복에서 2.4–3.8 µs를 줄이고 규칙을 6→3으로 줄인다.
3. 완료마다 새 execution turn(S01 §11·S08 §4.4)은 세 언어가 같이 지불하는 0.9–2.2 µs/request이며 어느 언어도 request 셀에서 0.7을 넘지 못하는 공통 원인이다. 대안 B1이 이를 없애되 S02 §13의 한 사례를 약화한다.
4. 미확정 최우선 항은 Core I/O thread의 Δ_native다 — ccall2 재계산으로 target I/O1이 메시지당 약 18.8 µs(binding ≤4.0)라는 가설이 나오며, 사실이면 Framework 쪽 절감과 무관하게 처리량을 막는다. 1~7단계 전·후 같은 세션에서 thread별 CPU와 frame 수를 먼저 잰다.
5. 추천 순서는 측정 → 1~7단계 → A → B1 → (필요 시) C이며, A+C까지 해도 backpressure 1 KiB의 0.8은 가설 하한에서만 들어간다(0.71–0.84).

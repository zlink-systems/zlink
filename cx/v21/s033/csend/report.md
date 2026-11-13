# C++ send-saturation 예산 분석 (#3868)

Condition B의 무제한 5회 중앙값은 1 KiB Framework 177,827.6 msg/s, binding 623,336.5 msg/s다. F÷B는 0.2853이다. Framework는 5.623 µs/msg이며 사용자 지정 고정 예산 2.775 µs/msg를 2.848 µs/msg 초과한다. 4 KiB는 Framework 154,583.9 msg/s, binding 286,079.1 msg/s이며 F÷B 0.5404다.

| Payload | Framework 5회 msg/s | Binding 5회 msg/s | Framework 중앙값 µs/msg | Binding 중앙값 µs/msg | F÷B | 예산 µs/msg | 초과 µs/msg |
|---|---|---|---:|---:|---:|---:|---:|
| 1 KiB | 179084, 176209, 177828, 180999, 177419 | 610754, 605500, 623337, 631365, 625569 | 5.623 | 1.604 | 0.2853 | 2.775 고정 | 2.848 |
| 4 KiB | 153543, 152185, 159139, 154584, 154595 | 286079, 276093, 330109, 250904, 322317 | 6.469 | 3.496 | 0.5404 | 5.826, 동일 회기 binding×0.6 기준 | 0.643 |

이번 binding 중앙값으로 0.6을 다시 대입한 1 KiB 목표는 374,001.9 msg/s, 2.674 µs/msg이며 Framework는 2.950 µs/msg 초과한다. 본문의 고정 2.775 µs/msg 계산도 유지했다.

처리량은 각 구현 5회 중앙값의 비다. 4 KiB binding 범위는 250,904–330,109 msg/s로 넓다. 좋은 회차만 고르지 않았다. CPU 분해와 floor도 같은 작업에서 새로 측정했다.

## 1. 실제 경로

아래 경로는 remote node로 보내는 정상 send 한 건이다. 축약한 `runtime/`는 `framework/languages/cpp/framework/src/runtime/`, `contracts/`는 `framework/languages/cpp/framework/include/zlink/framework/contracts/`다. 같은 node에 보내는 local 경로와 request 경로를 섞지 않는다. Source caller는 `source_t::run_phase`가 실행하는 phase worker다. `state_lane`은 비어 있으면 호출 thread에서 turn을 수행하므로 thread 전환과 같은 뜻이 아니다. 공통 속도 profile에서 source phase worker는 TID 1961666, target ingress worker는 TID 1960048, application worker는 TID 1960047·1961668이다. Source/target Core I/O는 각각 TID 1960102·1960043이며, 아래 단계의 thread 역할을 이 TID에 대응시켰다.

| 쪽 | 단계 | 위치 | 실행 thread |
|---|---|---|---|
| Source | typed payload 생성, stamp, send call 생성과 `.async()` | `framework/bench/grpc/cpp/client/bench_cpp_client.cpp:870`, `:881`, `:940` (측정 pacer 추가 전 위치) | phase worker |
| Source | typed serializer를 빌린 encoder로 보관하고 public terminal 제출 | `framework/languages/cpp/framework/include/zlink/framework/contracts/channels/channel.hpp:834`, `:850`; `contracts/channels/call.hpp:338` | phase worker |
| Source | transport snapshot 읽기, encode, mesh submit, public 결과 projection | `framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:1441`, `:1458`, `:1482`, `:1488` | phase worker |
| Source | protobuf를 binding native buffer에 직접 쓰고 native body를 move | `framework/languages/cpp/extensions/framework-codec-protobuf/include/zlink/codecs/protobuf.hpp:20`; `framework/languages/cpp/framework/include/zlink/framework/contracts/codecs/serializer.hpp:222`; `runtime/messaging/client_call_codec.cpp:67` | phase worker |
| Source | local RID 비교 후 remote send coroutine | `framework/languages/cpp/framework/src/runtime/host/app.cpp:2602`, `:2617` | 즉시 경로는 phase worker |
| Source | target의 논리 object role 확인 | `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:4274`, `:4282`; `runtime/mesh/raw_mesh_node_owner.cpp:4369` | phase worker |
| Source | public host에서 application payload ownership 이동 | `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:3373`, `:3377` | phase worker |
| Source | nodeSend service header, multipart를 한 service payload frame에 작성 | `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1277`, `:1282`, `:1758`, `:1781`; `runtime/protocol/service_wire_codec.cpp:3224` | phase worker |
| Source | owner turn에서 physical submit 시작 | `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1651`, `:1664`, `:1684` | `run_sync`의 호출 thread; 경쟁 시 기존 owner 차례를 기다림 |
| Source | socket lifetime mutex 아래 binding operation 작성과 async terminal | `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:151`, `:185`, `:196`, `:204` | 같은 caller turn |
| Source | binding 즉시 DONTWAIT 시도, BACKPRESSURED 때만 native completion 소유 | `bindings/cpp/src/Runtime/Messaging/send_operations.cpp:30`, `:60`; `bindings/cpp/src/Runtime/Messaging/operation_submit.hpp:64` | 즉시 제출은 phase worker; pending completion은 기존 poller 구동 thread |
| Transport | Core I/O가 TCP를 보내고 받음 | Core header를 읽지 않고 profile의 TCP·Core I/O symbol로 확인 | source/target Core I/O thread |
| Target | binding receive로 whole record 수신 | `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:459`, `:467`, `:484`; `bindings/cpp/src/Runtime/Sockets/router.cpp:32`, `bindings/cpp/src/Runtime/Sockets/socket.cpp:358`; `bindings/cpp/src/Runtime/Native/native_receive.hpp:163` | hosted ingress 역할을 가진 executor worker |
| Target | peer admission, nodeSend shape 확인과 mailbox enqueue | `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:4130`, `:4148`, `:4221`, `:4237` | 같은 ingress turn |
| Target | 기존 receive permit을 native record에 전달하고 mailbox ready owner 게시 | `framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp:85`, `:100`; `runtime/mesh/mesh_node_host_service.cpp:3364` | ingress worker |
| Target | owner drain을 claim하고 native record batch를 가져옴 | `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:3852`, `:3873`; `runtime/stateful/public_host_runtime.cpp:8542`, `:8563` | application executor worker; 현재 owner 전체 drain 하나 |
| Target | 하나의 service payload storage를 두 external native part로 투영 | `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:8437`; `runtime/protocol/service_wire_codec.cpp:3406` | 같은 application worker |
| Target | envelope header decode, Spot/internal probe 뒤 Node fallback dispatch | `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:3742`, `:3778`, `:3789`, `:3809`; `runtime/spots/spot_runtime.cpp:15692`; `runtime/mesh/mesh_record_dispatcher.cpp:33`, `:68` | application 역할을 얻은 executor worker |
| Target | handler lookup, filter/DI, typed decode, command handler 호출 | `framework/languages/cpp/framework/src/runtime/channels/route_packet_dispatcher.cpp:203`, `:232`, `:340`; `runtime/channels/route_handler_invoker.cpp:10`, `:44`; `framework/bench/grpc/cpp/framework/bench_framework_cpp_server.cpp:34` | handler execution turn의 worker |

## 2. CPU 분해와 동일 속도 CPU

총 CPU는 두 process의 `/proc/<pid>/stat` user+kernel tick 차이를 같은 관찰창의 target 수신 건수로 나눈 값이다. Source와 target의 모든 thread가 포함된다. Perf·collector와 process 밖 kernel worker의 CPU는 포함하지 않는다. `CLK_TCK=100`이다. 아래는 Framework를 무제한 실행하고 그 관찰 속도에 binding만 pace한 세 쌍이다. 각 capture는 active 구간 안의 약 2초다.

| 쌍 | 구현 | 관찰 msg/s | Source user µs/msg | Source kernel µs/msg | Target user µs/msg | Target kernel µs/msg | 두 process 합계 µs/msg |
|---|---|---:|---:|---:|---:|---:|---:|
| 1 | Framework | 166929.494 | 6.156 | 4.406 | 10.445 | 4.435 | 25.441 |
| 1 | Binding | 166909.497 | 1.521 | 1.112 | 1.463 | 0.819 | 4.914 |
| 2 | Framework | 178944.845 | 5.808 | 4.268 | 9.941 | 3.728 | 23.744 |
| 2 | Binding | 178954.353 | 1.569 | 1.136 | 1.380 | 1.028 | 5.114 |
| 3 | Framework | 181722.589 | 5.891 | 4.174 | 10.011 | 3.725 | 23.800 |
| 3 | Binding | 181699.354 | 1.518 | 1.066 | 1.385 | 0.959 | 4.929 |

쌍별 Framework 추가 총 CPU는 20.527, 18.631, 18.872 µs/msg다. 그 중앙값은 18.872 µs/msg다. 속도가 다른 두 구현의 포화 CPU를 빼서 이 값을 만들지 않았다. 공통 149,777 msg/s로 두 구현 모두 pace한 대조 3쌍도 측정했다. 그 총 CPU 중앙값은 Framework 23.886, binding 4.991 µs/msg이며 쌍별 차이 중앙값은 18.895 µs/msg다. 이 총 CPU 차이를 직렬 처리시간 예산 초과 2.848 µs/msg와 합치지 않는다.

동일 속도 단계별 비교는 Framework 149,881.9 msg/s와 binding 149,777.4 msg/s의 profile을 사용한다. Source phase worker는 Framework TID 1961666, binding TID 1964200이다. Core I/O는 각각 TID 1960102, 1962475다.

| CPU 범주 (µs/msg) | F source caller | B source caller | F source Core I/O | B source Core I/O |
|---|---:|---:|---:|---:|
| Framework bookkeeping | 0.990 | 0.000 | 0.000 | 0.000 |
| Encode·serialize | 0.796 | 0.049 | 0.000 | 0.000 |
| Allocation·free | 0.700 | 0.362 | 0.419 | 0.287 |
| Copy 심볼 | 0.200 | 0.031 | 0.218 | 0.133 |
| Binding·Core call | 1.547 | 0.696 | 4.299 | 1.009 |
| Wakeup·handoff | 0.227 | 0.006 | 0.036 | 0.009 |
| Bench driver | 0.512 | 0.120 | 0.000 | 0.000 |
| 기타 | 0.000 | 0.000 | 0.000 | 0.000 |
| 합계 | 4.972 | 1.264 | 4.973 | 1.438 |

Target ingress는 Framework TID 1960048이다. Application worker TID 1960047·1961668은 같은 Node owner drain 역할을 번갈아 실행한다. Binding TID 1962434는 `command_loop` 하나에서 receive·protobuf decode·handler 계측을 수행한다. Core I/O는 각각 TID 1960043, 1962429다.

| CPU 범주 (µs/msg) | F ingress | F application worker 합계 | B receive·handler | F target Core I/O | B target Core I/O |
|---|---:|---:|---:|---:|---:|
| Framework bookkeeping | 1.395 | 2.833 | 0.000 | 0.000 | 0.000 |
| Encode·serialize | 0.038 | 1.299 | 0.052 | 0.000 | 0.000 |
| Allocation·free | 0.469 | 0.765 | 0.048 | 0.252 | 0.053 |
| Copy 심볼 | 0.031 | 0.066 | 0.042 | 0.265 | 0.047 |
| Binding·Core call | 2.060 | 0.803 | 0.853 | 3.420 | 1.046 |
| Wakeup·handoff | 0.309 | 0.281 | 0.039 | 0.108 | 0.106 |
| Bench driver | 0.000 | 0.000 | 0.055 | 0.000 | 0.000 |
| 기타 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| 합계 | 4.303 | 6.047 | 1.088 | 4.045 | 1.251 |

Application worker 합계 6.047 µs/msg는 두 worker의 동시 실행을 뜻하지 않는다. 현재 Node owner drain의 직렬 처리권 때문에 하나의 직렬 자원이다. 무제한 profile의 관찰 속도 153,511.2 msg/s에서는 application worker 합계가 6.308 µs/msg·0.968 core였다. Source caller는 5.988 µs/msg·0.919 core, source Core I/O는 5.949 µs/msg·0.913 core, target ingress는 5.112 µs/msg·0.785 core였다. Source와 target의 이 자원들은 서로 병렬이므로 CPU를 더해 wall 처리시간으로 부르지 않는다. 같은 무제한 Framework profile 속도로 pace한 binding profile은 153,511.4 msg/s였다.

표의 임계 역할 외 stats·control·idle worker도 process CPU 합계에 포함된다. Allocation은 malloc/new/free와 식별된 kernel slab allocation이고, copy는 memcpy/memmove와 식별된 kernel copy다. 분류는 배타적이다. 작은 인라인 복사는 별도 copy 심볼에 잡히지 않을 수 있다. 기록된 active 구간 callchain 비율을 thread의 `schedstat` CPU 관찰창에 정규화했다. Perf 기본 시계와 Python 관찰 시계의 절대값을 직접 필터링하지 않았다. 이 표는 함수별 latency가 아닌 CPU 표본 추정값이다. Profile 처리량을 무제한 5회 결과에 대입하지 않는다.

## 3. Floor

두 floor 모두 원래 Framework target 바이너리를 사용했다. 첫 정상 send에서 얻은 실제 native service frame 두 개를 해당 phase 동안 재생한다. Target은 binding receive, service-wire decode, JSON header 처리, protobuf decode, 실제 command handler를 모두 수행한다. Source의 typed payload 생성·stamp는 남겼지만, 재생 frame의 seq·timestamp는 첫 값이다. 이 때문에 floor의 latency는 비교하지 않는다. 이 재생은 생략 비용을 보는 측정 도구이며 제품용 cache 제안이 아니다.

Public floor는 `send_to_node(...).async()`의 public call 생성과 일회 제출 claim을 남기고 `submit_send_erased` 진입 뒤 Framework 작업을 생략한다. Direct floor는 같은 bench loop가 binding `socket.send(rid).message(...).async()`를 바로 호출한다. 두 모드 모두 binding이 native admission과 BACKPRESSURED 재제출을 소유하고, 같은 기존 poller를 사용한다. Bench의 task 소비 모양을 유지하기 위해 native admitted stage를 기존 Framework task로 받는다. 따라서 direct floor에도 이 작은 bridge가 남는다. 일반 raw binding bench도 각 floor 회차에 교대로 실행했다.

| 1 KiB 무제한, 각 3회 | Framework target까지의 msg/s | 중앙값 µs/msg | 2.775 µs/msg 초과 | 같은 회기 raw binding 중앙값 msg/s | F÷B |
|---|---|---:|---:|---:|---:|
| Public send 유지 | 199,817.0, 203,378.0, 206,877.0 | 4.917 | 2.142 | 618,264.5 | 0.3289 |
| Bench → binding 직접 | 202,623.4, 206,877.0, 205,367.6 | 4.869 | 2.094 | 620,818.9 | 0.3308 |

다음 CPU 표는 모두 약 149,777 msg/s다. Floor public은 149,772.9 msg/s, direct는 149,775.8 msg/s였다. 원래 Framework와 raw binding은 앞 절의 같은 속도 profile이다.

| µs/msg | Source caller | Source Core I/O | Target ingress | Target application 합계 | Target Core I/O | 두 process user+kernel |
|---|---:|---:|---:|---:|---:|---:|
| 원래 Framework | 4.972 | 4.973 | 4.303 | 6.047 | 4.045 | 24.702 |
| Public floor | 1.370 | 1.588 | 2.311 | 4.833 | 1.301 | 11.592 |
| Direct floor | 1.138 | 1.433 | 2.146 | 4.706 | 1.251 | 10.852 |
| Raw binding | 1.264 | 1.438 | 1.088 (handler 포함) | 동일 thread | 1.251 | 5.198 |

Public shape의 관찰된 source caller floor는 1.370 µs/msg다. Direct와의 caller 차이는 0.232 µs/msg다. Public shape 자체가 source의 2.775 µs/msg 예산을 막는다는 증거는 없다. 반면 direct floor의 target application CPU도 4.706 µs/msg이며, 무제한 처리시간은 4.869 µs/msg다. Source Framework 작업 대부분을 생략해도 target의 현재 실행 구조가 남는다.

같은 평균 전송률에서도 floor는 빠르게 제출한 뒤 쉬는 분포와 재생 storage를 사용한다. 원래 Framework와 floor의 Core I/O·target CPU 차이를 전부 source Framework 함수의 삭제량으로 계산하지 않았다. 같은 Framework wire를 재생할 때 source Core I/O가 4.973에서 1.433–1.588 µs/msg로 줄었다는 관찰만 사용한다. Binding/Core 비용을 고정 상수로 두거나 Framework queue로 batch 동작을 다시 만들지 않는다. 모든 Framework floor cell에서 첫 한 건을 제외한 완료 건수가 실제 직접 binding 호출 횟수와 일치했다.

## 4. 예산과 가장 작은 완전 변경안

아래는 같은 속도 Framework profile의 배타적 scope CPU다. `F/`는 `framework/languages/cpp/framework/`다. Framework가 추가한 경로를 모두 적고 공통 native submit·receive와 protobuf·bench 비용은 뒤에서 따로 적었다. 이 표는 앞 절의 allocation·copy 분류와 같은 CPU를 다른 기준으로 나눈 것이므로 두 표를 더하지 않는다. Scope 전체를 삭제할 수 있다는 뜻도 아니다.

| Framework scope | Source µs/msg | Target ingress µs/msg | Target application µs/msg | 위치와 남길 동작·제거할 표현 |
|---|---:|---:|---:|---|
| public send/transport lookup | 0.500 | — | 0.003 | F/include/zlink/framework/contracts/channels/channel.hpp:834; contracts/channels/call.hpp:338; F/src/runtime/channels/channel_runtime.cpp:118. Public 일회 제출과 transport 조회는 유지한다. |
| serializer selection | 0.081 | — | — | F/include/zlink/framework/contracts/codecs/serializer.hpp:245. 기존 serializer plan을 사용하며 새 cache를 두지 않는다. |
| envelope metadata lifetime | 0.051 | 0.016 | 0.307 | F/src/runtime/messaging/client_call_codec.hpp:63; envelope_codec.cpp:496. Header 소유 표현을 한 번 만든다. |
| JSON header writing | 0.308 | — | 0.036 | F/src/runtime/messaging/envelope_codec.cpp:496,509,529. 중간 native header buffer를 없애고 최종 buffer에 쓴다. |
| service payload packing | 0.329 | — | — | F/src/runtime/protocol/service_wire_codec.cpp:3224. 현재 wire 형식의 한 번 쓰기는 유지한다. |
| payload container/projection | 0.171 | 0.009 | 0.057 | F/src/runtime/messaging/envelope_codec.cpp:44; host/app.cpp:2617; stateful/public_host_runtime.cpp:3373. take_items 왕복을 없애고 기존 owned encoder를 전달한다. |
| other envelope/codec work | 0.640 | 0.019 | 0.084 | F/src/runtime/messaging/client_call_codec.cpp:67; client_call_codec.hpp:55. Typed encode와 header·wire 조립의 소유를 연결한다. |
| mesh ownership/topology | 0.284 | 0.391 | 0.009 | F/src/runtime/mesh/mesh_node_runtime.cpp:4282; raw_mesh_node_owner.cpp:1765,4369. Role·readiness 판정은 유지하고 중복 조회와 RID 표현 왕복을 정리한다. |
| Framework port/lifetime gate | 0.156 | 0.888 | — | F/src/runtime/backend/raw_route_port.cpp:185,459,481. Socket close 수명 보호는 유지한다. Native RID·frame 표현 재생성을 줄인다. |
| lane/dispatch/permit | 0.093 | 0.088 | 0.128 | F/src/runtime/mesh/raw_mesh_node_owner.cpp:1652; mesh_node_host_service.cpp:3364. Logical owner turn과 permit은 유지한다. |
| task/result projection | 0.233 | 0.057 | 0.167 | F/src/runtime/host/app.cpp:2602; channels/channel_runtime.cpp:1489; contracts/dispatch/task.hpp:1448. Result carrier를 한 번 적응한다. 기존 ready fast path는 이미 있다. |
| ambient/flow context lifetime | 0.057 | 0.009 | 0.042 | F/src/runtime/channels/channel_runtime.cpp:1453; contracts/dispatch/task.hpp. Context 수명 계약을 유지한다. |
| ingress readiness/permit drive | — | 0.517 | — | F/src/runtime/stateful/public_host_runtime.cpp:7553; mesh/mesh_node_host_service.cpp:2097. 기존 readiness·receive turn을 사용하고 두 번째 poller를 만들지 않는다. |
| mailbox claim/credit/owner drain | — | 0.633 | 0.568 | F/src/runtime/mesh/service_mailbox.cpp:85,146,172,453. FIFO·credit은 유지한다. Node owner 전체의 실행 배타권은 제거해야 한다. |
| incoming record ownership/projection | — | — | 0.143 | F/src/runtime/stateful/public_host_runtime.cpp:8437. Native record를 기존 claim에서 소유하고 재포장 단계를 줄인다. |
| application claim handoff | — | — | 0.308 | F/src/runtime/stateful/public_host_runtime.cpp:8542. Prefix 참조 뒤 다시 take하는 표현을 단일 record 소유권 이전으로 바꾼다. |
| application record preparation | — | — | 0.251 | F/src/runtime/mesh/mesh_node_host_service.cpp:3742. 기존 terminal·handler entry의 소유권을 한 record에 모은다. |
| route dispatch | — | — | 1.108 | F/src/runtime/mesh/mesh_node_host_service.cpp:3789,3809; spots/spot_runtime.cpp:15692; channels/route_packet_dispatcher.cpp:25,203. Internal packet과 public Node의 분류를 보존하며 route·context 표현을 정리한다. |
| handler invocation/lifetime | — | — | 0.918 | F/src/runtime/channels/route_handler_invoker.cpp:10,38,44; contracts/configuration/mesh_node.hpp:574. Typed handler와 async 수명을 보존하며 wrapper 수를 줄인다. |
| handler scope/DI | — | — | 0.224 | F/src/runtime/configuration/services.cpp:254,264. Handler scope 격리는 유지한다. Registry 전체 복사가 아니다. |
| handler terminal/accounting | — | — | 0.278 | F/src/runtime/channels/route_handler_invoker.cpp:63; mesh/mesh_node_host_service.cpp:3760. 완료·physical cleanup 소유는 유지한다. |
| header parsing/validation | — | 0.016 | 0.822 | F/src/runtime/mesh/mesh_node_host_service.cpp:3778; messaging/envelope_codec.cpp:1266. 정상 header 해석·검증 한 번은 유지한다. |
| service unpack/native part projection | — | — | 0.233 | F/src/runtime/protocol/service_wire_codec.cpp:3365,3400. 기존 view decoder를 사용해 두 external native part 생성을 없앤다. |
| body projection | — | — | 0.066 | F/src/runtime/channels/route_packet_dispatcher.cpp:262; messaging/envelope_codec.cpp:1282. 기존 encoded_payload view를 typed decode에 전달해 추가 native retain을 줄인다. |
| other Framework | 0.212 | 0.180 | 0.078 | 표본상 남은 소규모 Framework helper다. 삭제 가능량에 산입하지 않는다. |

Source caller의 공통 native async submit은 1.023, native builder는 0.123, protobuf/lifecycle은 0.141, bench scope는 0.565 µs/msg다. Target ingress의 native receive/materialization은 1.477, target application의 공통 protobuf/lifecycle은 0.212 µs/msg다. 이들을 Framework-only 삭제 예산으로 세지 않았다. Native call 안의 비용이 rate·제출 분포·record 소유 방식에 따라 달라질 수 있다는 제한도 남긴다.

현재 CPU 비용을 고정한 단일 직렬 자원 모델에서 source caller 4.972 µs/msg는 예산을 2.197 µs/msg 초과하고, target application 6.047 µs/msg는 3.272 µs/msg 초과한다. Direct floor의 target application 4.706 µs/msg도 1.931 µs/msg 초과한다. 이는 후속 변경에서 확인할 직렬 자원별 조건이며, 서로 더한 wall 예측값이 아니다. 무제한 실제 초과분은 앞의 2.848 µs/msg다.

대안 A는 send header의 최종 buffer 작성만 합치고 결과 adapter를 정리하는 것이다. Header 작성·service packing·task/result scope 전체를 0으로 잡아도 현재 source 기준 0.871 µs/msg뿐이다. 필수 wire 쓰기와 결과 계약을 남기면 실제 제거량은 이보다 작다. 더 강한 생략인 direct floor도 2.094 µs/msg 초과하므로 이 부분 패치만으로 목표를 만족한다는 제안은 하지 않는다.

추천하는 대안 B는 public API를 유지하고 binding native record를 송수신 양쪽의 소유 단위로 쓰는 것이다. Source에서는 cpr2의 기존 `application_envelope_encoder_t`와 `take_application_payload()`를 send에도 연결해 최종 buffer 한 곳에서 조립한다. `prepare_request_type`의 request 전용 kind를 command에 그대로 사용하지 않고, 기존 header writer에서 올바른 kind를 쓰도록 일반화해야 한다. Native body serializer는 이미 binding buffer에 직접 쓰므로 다시 구현하지 않는다. Admission·DONTWAIT 재제출·physical 완료는 기존 binding `send(...).async()`가 계속 소유한다. Framework는 logical role·readiness·close와 public terminal만 소유한다.

Target에서는 one-way void handler가 `task_t<message_t>`의 빈 native message를 만든 뒤 optional reply로 옮기는 내부 결과 표현도 한 terminal로 정리한다(`contracts/configuration/mesh_node.hpp:594`, `route_handler_invoker.cpp:36,49`, `route_packet_dispatcher.cpp:203`). Public filter·typed handler 계약과 async cleanup 수명은 유지해야 한다. 기존 native record와 기존 terminal/claim의 수명을 유지하고 `decode_application_part_views`·`decode_body_view`를 연결한다. Payload storage를 두 external message로 투영했다가 body를 다시 retain하는 표현을 줄인다. Node·Channel은 기존 FIFO에서 record를 배타적으로 가져온 뒤 기존 application worker들이 독립 실행해야 한다. Source와 target에 새 queue·lock·cache·timer·flag를 추가하는 안은 제안하지 않는다.

이때 `active_messages != 0`만 삭제하면 안 된다. `service_mailbox.cpp:199`는 claim이 FIFO prefix의 주소를 가리킨다고 명시하고, `:207`에서 그 주소를 저장하며 `:409`에서 마지막 settle 뒤 prefix를 지운다. Guard만 지우면 다음 worker가 같은 prefix를 다시 claim할 수 있다. 기존 list node를 기존 claim의 record 소유로 옮기고 credit·terminal·close의 귀속을 같이 정리해야 한다. 이것이 단순 병렬 worker 추가보다 먼저 필요한 구조 변경이다.

| 결정 | 계약 | 현재 판정 위치·소유자 | 제안 뒤 소유자 |
|---|---|---|---|
| 최종 service payload용 buffer 할당 | S08 §3 E1, `08-messaging-hot-path.ko.md:96` | header codec `envelope_codec.cpp:506` + service codec `service_wire_codec.cpp:3224` | 기존 service wire writer 한 곳. Header codec은 그 span에 기록한다. |
| Physical admission·retry | S08 E4, `:99`; binding `operation_contracts.hpp:215` | Binding `send_operations.cpp:30,60`와 Core | 그대로 binding/Core. Framework retry·send queue를 만들지 않는다. |
| Node owner 전체의 실행 진입 거절 | S02 §2, `02-handler-turn-and-execution-gate.ko.md:97` | `service_mailbox.cpp:158,181,453` | Owner 전체 gate는 없애고 기존 FIFO take와 record별 terminal이 소유한다. Spot·Actor gate는 유지한다. |
| Payload physical 수명 | S05 payload ownership; S02 handler turn | Claim에서 `retained_application_message_t`로 넘긴 뒤 external parts가 참조 | 기존 native record와 claim/physical terminal이 소유한다. 별도 backing owner를 늘리지 않는다. |
| Public 완료 | S01 submit/completion; S08 E3·E5 | 즉시 send는 `raw_mesh_node_owner.cpp:1685`에서 반환. Pending만 registry 등록 | 즉시·pending 계약을 유지한다. Result 표현 수만 줄인다. |

제안 범위의 판정 위치 수는 Header·최종 service payload용 buffer 할당 2→1, Node owner 전체의 진입 거절 3→0으로 합계 5→1이다. Physical retry와 public 완료의 권한은 각각 기존 한 소유자로 유지한다. 이 수치는 제안의 범위이며 실제 측정본의 제품 runtime은 수정하지 않아 5→5다.

교차언어 정적 대조에서 .NET의 `ZLinkManagedMeshNode.cs:3413,12264,12344`, Java/Kotlin 공통의 `ZLinkChannelRouteCalls.java:187`·`ZLinkJavaRawSpotNode.java:329`·`ZLinkJavaRawMeshNode.java:2199`, Node의 `channel-clients.ts:202`·`raw-service-mesh-runtime.ts:641,2124`도 Framework 논리 송신에서 binding submit으로 내려간다. 이 작업에서 교차언어 runtime을 수정하거나 parity test를 실행하지 않았다. 후속 구현은 기존 Node 독립 실행 계약과 native body/terminal 수명에 맞춰 contract test로 확인해야 한다.

제품 prototype은 작성하지 않았다. 예산을 충족할 가능성이 있는 최소 완전안은 source의 owned encoder 전달 경계와 target의 prefix claim·record 수명·실행 권한을 함께 바꿔야 한다. 로컬/remote·미발견 target·BACKPRESSURED·close·taken-before-entry·async handler cleanup·gated Actor 음성 대조까지 보존하는 작업을 3–4시간으로 판단했다. 2시간 이내의 header-only 부분 패치는 위 예산과 floor를 충족하지 않는다. 따라서 제품 prototype의 RUNS=5 1 KiB·4 KiB 결과와 `prototype.diff`는 없다. 위 floor 수치를 제품 개선 수치로 제시하지 않는다. 제안 B가 0.6을 달성한다는 측정 결과도 아직 없으며 완전 prototype에서 확인해야 한다.

소유 계층은 Framework의 packet·record 표현과 logical execution이다. Physical submit·retry는 binding/Core다. 계약 근거는 S08 E1/E4, S02 §2, S05 payload ownership이다. 제안 분류는 B인 기존 중간 표현·Node owner 배타권 정리이며 새 계약을 추가하는 D 변경을 제안하지 않는다. 구현 전 physical cleanup와 claim 이동의 교차언어 계약 검증이 남는다.

### 중복 작업 점검

아래 `runtime/`는 `F/src/runtime/`다. 제품 prototype이 없으므로 아래 항목을 제품 코드에서 제거했다고 표시하지 않는다.

| 관찰 | 위치 | 판정과 처리 |
|---|---|---|
| JSON header native storage 작성 뒤 최종 service payload에 재복사 | `runtime/messaging/envelope_codec.cpp:496,509`; `runtime/protocol/service_wire_codec.cpp:3199,3224` | 중간 header 표현 제거 후보다. Floor source에서 생략했으며 제품 구현은 없다. |
| 같은 peer snapshot 반복 조회 | `runtime/mesh/raw_mesh_node_owner.cpp:4369,4373,1765`; `runtime/mesh/service_topology_registry.cpp:515` | Role과 readiness는 다른 결정이다. Immutable snapshot alias 조회가 반복되며 deep copy는 아니다. 미구현이다. |
| RID를 bytes로 바꾼 뒤 native RID로 다시 작성 | `runtime/stateful/public_host_runtime.cpp:3377`; `backend/raw_route_port.cpp:196,481` | Native RID를 경계까지 보존할 후보다. Floor source에서 생략했으며 제품 구현은 없다. |
| 즉시 결과를 coroutine·nested result·public projection으로 표현 | `runtime/host/app.cpp:2602,2617`; `channels/channel_runtime.cpp:1489`; `contracts/dispatch/task.hpp:1448` | Projection의 ready 경로는 이미 inline이다. Pending registry 비용을 모든 send의 비용으로 세지 않았다. 미구현이다. |
| Service payload를 두 external part로 투영하고 body를 추가 retain | `runtime/protocol/service_wire_codec.cpp:3400`; `channels/route_packet_dispatcher.cpp:262`; `messaging/envelope_codec.cpp:1282` | 같은 backing storage를 공유한다. 두 번의 payload deep copy가 아니다. 기존 view decoder 재사용 후보이며 미구현이다. |
| 같은 active claim 검사와 owner drain 진입 조건 | `runtime/mesh/service_mailbox.cpp:158,181,453` | Prefix 보관 방식과 함께 바꿔야 한다. Guard만 제거하는 prototype은 만들지 않았다. |
| Spot/internal dispatch 검사 뒤 Node dispatcher로 진행 | `runtime/mesh/mesh_node_host_service.cpp:3789,3809`; `spots/spot_runtime.cpp:15692` | 실제 정상 Node 경로에도 probe가 있다. Internal control packet 소유를 보존해야 하며 단순 skip하지 않았다. |
| One-way void 결과를 빈 native message와 optional reply로 차례로 표현 | `contracts/configuration/mesh_node.hpp:594`; `runtime/channels/route_handler_invoker.cpp:36,49`; `route_packet_dispatcher.cpp:203` | Target의 기존 request 공통 carrier를 사용하는 비용이다. Public filter 계약을 보존하는 내부 terminal 정리 후보이며 미구현이다. |
| Raw binding bench의 protobuf string 뒤 native body 복사 | `framework/bench/grpc/cpp/common/bench_common.hpp:160`; `bench_cpp_client.cpp:618` | Framework protobuf는 `framework/languages/cpp/extensions/framework-codec-protobuf/include/zlink/codecs/protobuf.hpp:20`에서 native buffer에 직접 쓴다. Raw reference를 임의로 바꾸지 않았다. |

Refactoring 점검은 위 9항목이며 제품 수정 0, 후속 검토 9다. 기본 no-filter 경로는 `handlers/handler_registry.cpp:350`에서 이미 terminal을 바로 호출한다. DI `services.cpp:264`는 immutable registry를 공유하고 handler scope state 하나를 만들며 registry 전체를 복사하지 않는다. Source state lane과 port lifetime mutex도 보호 대상이 달라 같은 lock 두 개로 단정하지 않았다.

### 검증과 재현 자료

사용자가 지정한 d961e51e23 측정 worktree와 기존 CPU pin 조건을 사용했다. Source는 CPU 0–7, target은 8–15이며 모든 capture thread affinity를 검사했다. Release `-O3 -DNDEBUG -g -fno-omit-frame-pointer`로 세 baseline 실행 파일과 floor client를 빌드했다. C++ 빌드는 지정한 `cpp-build-slot.sh`를 바깥 flock 없이 사용했다. 측정은 exclusive `measure.lock` 안에서 수행했고, warmup 30초·10 segment, 무제한 active 5초, profile active 20초·CPU 관찰 약 12초였다. Source/target을 각 cell 뒤 SIGTERM으로 종료했다. 제품 runtime·binding·Core·spec 변경은 남기지 않았다.

51개 cell에서 error·abandoned·header validation failure·target reject·drain bound·contamination이 없고, submitted=completed=received였다. 측정 집계 검증기에 오류·포기·header 실패·오염·drain 초과·송수신 불일치·reject·source 불일치·target 오류·0 처리량을 주입한 10종은 모두 거부됐다. Runtime unit/E2E gate는 실행하지 않았다. 제품 prototype이 없으므로 그 정확성·성능 검증은 미실행이다.

원시 결과와 재현 코드는 `baseline/`, `profile-*/`, `matched-*/`, `peak-*/`, `floor-*/`, `scripts/`에 있다. 최종 CPU 분해는 `analysis.json`, 집계 검증은 `quality.json`, 동일 속도는 `rate-matches.json`·`peak-complete.json`이다. `package-hashes.json`과 바이너리 hash 두 파일이 Core·binding·target 동일성을 기록한다. Floor 도구는 `floor.diff`와 `floor-source/`, pacing 도구는 `pacing.diff`로 보존했다. 원래 target 두 실행 파일은 두 빌드에서 byte-identical이다.

측정 중 최소 MemAvailable은 25.110 GiB, 빌드 전체 관찰 최소는 12.973 GiB였다. `final-checks.json`과 `package-hashes-verified.json`에서 확인했다. 모든 측정 삽입은 worktree에서 복원했고 기존 CPU pin 변경 네 파일만 남겼다. 남은 실행 실패는 없다.

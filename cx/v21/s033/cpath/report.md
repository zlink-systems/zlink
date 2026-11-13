# C++ Framework 메시지 경로 설계 — #1639

결론은 **6개 셀의 예산 충족을 아직 입증하지 못했다**는 것이다. Binding 호출에 logical admission, wire, application permit, 실행 순서, dispatch scope와 logical completion만 추가하는 구조로 설계했다. Coroutine은 유지한다. 아래 가정 범위에서 4 KiB의 CPU 합계는 들어가지만, 1 KiB send의 target CPU와 1 KiB request-backpressure의 caller 비용은 상단 추정에서 초과한다. Serial은 CPU 합계 외에 scheduler 대기 시간이 남는다. 이 보고서는 구현 승인을 요청할 수 있는 검토 자료이며 구현 완료나 성능 합격 보고가 아니다.

작업 기준은 `532cd83b67cdb4ec2a4ab9261d7a27459a376a37`이다. Worktree 변경은 없다. 입력 파일을 읽어 산술을 재계산한 Python 파일과 중간 JSON만 이 보고서 옆 `analysis/`에 작성했다. 빌드, test, 새 실행 측정, lock 획득, spec 수정과 GitHub 작업은 하지 않았다. 과거 보고서 안의 실행 지시와 경로는 실행하거나 따라 읽지 않았다.

표의 경로 접두어는 다음과 같다. 모든 코드 위치는 현재 worktree 기준이다. 과거 profile의 symbol은 현재 코드에 같은 구현이 있다는 증거로 사용하지 않았다.

| 접두어 | 경로 |
|---|---|
| C | `framework/languages/cpp/framework/src/runtime/` |
| H | `framework/languages/cpp/framework/include/zlink/framework/` |
| B | `bindings/cpp/` |
| P | `framework/bench/grpc/cpp/` |
| N | `framework/languages/dotnet/src/Zlink.Framework/Runtime/` |
| J | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/` |
| S01 | `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md` |
| S02 | 같은 디렉터리의 `02-handler-turn-and-execution-gate.ko.md` |
| S04 | 같은 디렉터리의 `04-application-job-queue-and-backpressure.ko.md` |
| S05 | 같은 디렉터리의 `05-payload-ownership-and-codec.ko.md` |
| S06 | 같은 디렉터리의 `06-state-ownership-and-lanes.ko.md` |
| S08 | 같은 디렉터리의 `08-messaging-hot-path.ko.md` |
| W | `framework/doc/framework/common/spec/server/02-channel-transport/06-wire-protocol.ko.md` |
| API | `framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md` |
| CPP | `framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md` |

## 1. 셀별 예산

`B`와 `F`는 `input/ratio051pr2/ratio051pr2/log/after/cpp/run{1..5}/…/results.json`에서 다시 구한 처리량 중앙값이다. 비율은 중앙값끼리 나눈다. 목표는 `G=max(0.6B, 이전 다른 언어 Framework 최고 처리량)`이고 간격 예산은 `T=10^6/G`다. 먼저 요청한 `0.6B` 예산을 계산하고 절대 목표가 더 엄격하면 함께 축소했다.

| 셀 | B msg/s | F msg/s | F÷B | 0.6B msg/s | 다른 언어 최고 msg/s | 최종 G·더 엄격한 조건 | T µs/message |
|---|---:|---:|---:|---:|---:|---|---:|
| send-saturation 1024 | 600,543.976 | 181,862.465 | 0.302830 | 360,326.386 | .NET 178,561 | 360,326.386 · 0.6 | 2.775262 |
| send-saturation 4096 | 278,281.817 | 157,778.762 | 0.566975 | 166,969.090 | .NET 130,583 | 166,969.090 · 0.6 | 5.989132 |
| request-backpressure 1024 | 249,414.635 | 51,549.452 | 0.206682 | 149,648.781 | Java 68,227 | 149,648.781 · 0.6 | 6.682313 |
| request-backpressure 4096 | 152,591.285 | 45,175.507 | 0.296056 | 91,554.771 | Java 61,535 | 91,554.771 · 0.6 | 10.922424 |
| request-serial 1024 | 16,143.192 | 7,907.153 | 0.489813 | 9,685.915 | Java 8,897 | 9,685.915 · 0.6 | 103.242696 |
| request-serial 4096 | 14,019.516 | 7,705.180 | 0.549604 | 8,411.709 | Java 8,695 | 8,695 · 절대 목표 | 115.008626 |

다른 언어 값은 **이전 세션**의 표다. Send는 `ratio040__ratio040-24cells.md`, request는 이 표와 `ratio053java__table.md`, `ratio054net__table.md`를 모두 비교했다. 같은 세션 비교나 현재 revision의 성능으로 해석하지 않는다. 다른 언어 원시 run은 제공되지 않아 표의 반올림된 msg/s가 절대 목표의 입력 정밀도다.

같은 thread의 binding 비용을 `b_thread`라 하면 Framework 추가 허용량은 `T-b_thread`다. 확인 가능한 source caller 비용은 `submit_thread_cores × active_elapsed_seconds × 10^6 / active_count`로 run마다 계산했다. Send의 active_count는 target receive, request는 source completion이다. 이 값은 thread의 **user+kernel CPU**이고 process user CPU가 아니다.

| 셀 | Binding caller CPU | Framework caller CPU | T−binding caller CPU | 병목과 근거 |
|---|---:|---:|---:|---|
| send 1024 | 1.343889 | 4.765876 | 1.431373 | Binding의 source I/O0가 유력하다. 별도 `ctrecv` unpaced binding에서 I/O0가 0.969–1.000 core이며 caller는 0.708–0.899다. Ratio051의 Framework caller는 0.854 core다. 이 세션의 I/O TID가 없어 caller와 I/O의 유일 병목은 확정할 수 없다. |
| send 4096 | 1.937687 | 5.189670 | 4.051445 | I/O 또는 target receive가 후보이며 확정 불가다. Ratio051 binding caller는 0.546 core, Framework caller는 0.824다. 다른 thread별 자료가 없다. |
| backpressure 1024 | 4.006277 | 17.412762 | 2.676037 | Source caller가 확인된 제한점이다. Binding 0.999 core, Framework 0.898 core다. 별도 ccall2 profile에서도 source caller가 0.824 core로 source 최댓값이며 target I/O1은 0.813이다. Target도 개선해야 한다. |
| backpressure 4096 | 6.405189 | 19.957120 | 4.517235 | Source caller가 제한점 후보이다. Binding 0.977 core, Framework 0.912 core다. Target TID가 없어 다른 제한점을 배제할 수 없다. |
| serial 1024 | 15.572439 | 19.024455 | 87.670257 | 단일 포화 thread가 아니라 window=1의 왕복 의존 경로가 병목이다. Caller는 binding 0.235 core, Framework 0.153 core다. |
| serial 4096 | 16.971464 | 19.508238 | 98.037161 | 같은 왕복 의존 경로다. Caller는 binding 0.240 core, Framework 0.152 core다. |

**확정되지 않은 thread의 비용을 caller 값으로 대체하지 않았다.** Ratio051에는 결과 JSON, source log, target log, target stats만 있고 target·I/O별 CPU가 없다. 따라서 send 4096 등의 정확한 병목 thread 추가 허용량은 `T−β_IO`, `T−β_receive`로 남는다. 위 caller 열은 그 thread에 대한 실제 제약이며 전체 병목의 증명이 아니다. 별도 `ctrecv__raw/raw/final/unpaced/binding-{1..5}/capture/{before,after}.json`의 schedstat 차이로 계산한 source I/O0 중앙값은 1.627517 µs/message, target receive+handler는 1.380896이다. 이를 send 1024의 진단에 대입하면 각각 1.147744, 1.394365 µs가 남지만 **다른 세션을 결합한 보조값**이다.

Serial의 binding 왕복 간격은 각각 61.945618, 71.329140 µs다. 실제 Framework 추가 지연 허용량은 **41.297078, 43.679486 µs**다. 위 caller 여유 87.670257, 98.037161을 왕복 여유로 사용하면 안 된다. Serial 4096의 0.6 전용 간격은 118.881899 µs이며 절대 목표가 이를 115.008626으로 줄인다.

Process CPU는 source에서 `client_cores`, target에서 `server_cpu_percent × logical_cores/100`을 active 구간 CPU로 되돌려 해당 구간 count로 나눈 뒤 중앙값을 취했다. `P/common/bench_common.hpp:256`은 user+kernel 합산이고 `P/client/bench_cpp_client.cpp:1230,1253,1272`가 구간과 count를 정한다. 각 process의 요구 예산은 `binding_CPU/0.6`, Framework 몫은 `binding_CPU×(1/0.6−1)`이다.

| 셀 | Source binding CPU | Source 0.6 예산 | Source Framework 몫 | Target binding CPU | Target 0.6 예산 | Target Framework 몫 |
|---|---:|---:|---:|---:|---:|---:|
| send 1024 | 2.932731 | 4.887885 | 1.955154 | 2.774138 | 4.623563 | 1.849425 |
| send 4096 | 5.440776 | 9.067960 | 3.627184 | 5.426318 | 9.043863 | 3.617545 |
| backpressure 1024 | 9.566212 | 15.943687 | 6.377475 | 9.376935 | 15.628224 | 6.251290 |
| backpressure 4096 | 16.889206 | 28.148676 | 11.259470 | 17.547248 | 29.245413 | 11.698165 |
| serial 1024 | 45.045867 | 75.076444 | 30.030578 | 36.058980 | 60.098299 | 24.039320 |
| serial 4096 | 47.738143 | 79.563571 | 31.825428 | 40.691335 | 67.818892 | 27.127557 |

Serial 4096에는 절대 목표도 적용한다. Source의 유효 예산/Framework 몫은 **76.971322/29.233179**, target은 **65.609295/24.917960 µs**다. 다른 셀은 위 0.6 예산과 같다. CPU가 여러 thread에 분산되므로 source+target process CPU를 T와 직접 비교하지 않는다.

별도 같은-rate 입력도 재계산했다. `csrc__raw/raw/paced/change-{1..3}`의 source user 중앙값은 3.958, `ctrecv__raw/raw/final/paced/step-d-{1..3}`의 target user는 6.775다. 이 숫자는 **process user**이며 caller 비용이 아니다. 같은 `ctrecv` binding의 process user+kernel 중앙값은 source 2.678, target 2.377이다. 이를 0.6으로 나눈 별도 같은-rate 예산/Framework 몫은 source 4.464/1.786, target 3.962/1.585다. Ratio051의 unpaced 예산과 혼합하지 않는다.

현재 S08 §7(c), `S08:396`은 0.90과 3-run 판정을 명시하고 request-backpressure의 bench 기준 0.80도 구별한다. 이번 지시는 0.60·5-run이므로 이 보고서는 지시된 계산을 사용한다. 0.60 달성을 현재 spec 전체 합격으로 표시하지 않는다. 이 기준 차이는 PM·대표님 결정 대상이다.

## 2. Binding의 메시지별 기준 작업

아래는 Core 내부 구현을 추측한 목록이 아니라 C++ binding에서 native 경계까지 확인한 작업이다. Serialization과 benchmark metrics는 binding API의 책임이 아니지만 비교 벤치 양쪽에 있으므로 비용에서 공통 기준으로 남긴다. 현재 코드는 `B`의 public API만 재사용한다.

| 경로 | 메시지마다 하는 작업 | 현재 file:line |
|---|---|---|
| send·request·reply 공통 | Operation state를 thread-local pool에서 꺼내고 종료 때 reset한다. Pool이 비었거나 동시 수명이 길면 할당한다. | `B/src/Runtime/Messaging/operation_state.hpp:301,322,333` |
| send | Socket runtime과 native RID를 operation에 연결한다. | `B/src/Runtime/Sockets/router.cpp:22` |
| send | 첫 part와 나머지 parts의 소유권을 operation에 이전한다. Lvalue는 실패 반환을 위한 source 주소도 보관한다. | `B/src/Runtime/Messaging/send_operations.cpp:135,151`; `B/src/Runtime/Messaging/operation_state.hpp:249,272` |
| send | `async()`가 DONTWAIT로 정확히 한 번 native submit한다. Socket call guard, RID 검증, native parts 변환, 결과·errno 판정을 수행한다. | `B/src/Runtime/Messaging/send_operations.cpp:174,58`; `B/src/Runtime/Messaging/operation_submit.hpp:29,55,95` |
| send 즉시 수락 | Source 참조를 해제하고 operation state를 반환하며 즉시 result를 만든다. 현재 `make_shared<immediate_send_result_t>` 한 번은 존재한다. | `B/src/Runtime/Messaging/send_operations.cpp:80` |
| send pending | Wait token, result와 completion entry를 묶고 socket completion owner에 등록한다. 재제출 중 원본 parts를 보관한다. | `B/src/Runtime/Messaging/send_operations.cpp:89,114`; `B/src/Runtime/Messaging/completion_owner.cpp:608` |
| send pending 완료 | Poller의 completion drain이 native completion을 받고 WRITABLE 재제출을 수행한다. 성공/실패를 한 번 확정하고 result를 완료한다. Framework가 이 일을 다시 하지 않는다. | `B/src/Runtime/Messaging/completion_owner.cpp:182,246,669,703,756` |
| receive | Reusable receive envelope와 caller의 parts 저장소를 준비한다. Native multipart를 한 번에 받고 BUFFER_TOO_SMALL일 때만 capacity를 늘린다. | `B/src/Runtime/Sockets/socket.cpp:366`; `B/src/Runtime/Native/native_receive.hpp:177` |
| receive | Native RID, reply token, route generation을 읽고 received 값에 commit한다. Reply/send context는 lazy runtime 정보이며 메시지별 closure를 만들지 않는다. | `B/src/Runtime/Sockets/socket.cpp:396`; `B/src/Runtime/Sockets/router.cpp:31` |
| receive 소유권 | Part는 native message를 소유한다. Move는 storage handle을 이전하며, copy는 `zlink_msg_copy`다. `to_bytes()`와 같은 body deep copy로 세지 않는다. | `B/src/Runtime/Messaging/message.cpp:36,72`; `B/src/Runtime/Messaging/received.cpp:21,117` |
| request | RID/runtime state와 timeout을 보관하고 admitted·reply·entry를 한 bundle에 만든다. Socket registry 등록 후 native request를 시작한다. | `B/src/Runtime/Sockets/router.cpp:83`; `B/src/Runtime/Messaging/request_reply.cpp:16,49,100,127`; `B/src/Runtime/Messaging/completion_owner.cpp:589` |
| request | `zlink_request`의 completion ID와 admission 결과를 연결한다. Timeout과 physical correlation, physical late reply 처리는 native/binding 결과를 따른다. | `B/src/Runtime/Messaging/operation_submit.hpp:130,164,184`; `B/src/Runtime/Messaging/completion_owner.cpp:385,521` |
| request 결과 전달 | Admitted가 pending이면 그것만 기다리고 reply를 기다린다. Binding await slot이 coroutine을 재개하며 poller가 completion entry를 제거한다. | `B/src/Runtime/Messaging/async_operation_state.hpp:18,51,126`; `B/src/Runtime/Messaging/completion_owner.cpp:521,722` |
| reply | Received의 RID/token을 socket-local owner와 검증하고 reply builder를 만든다. Native `zlink_reply`에 parts를 한 번 제출한다. | `B/src/Runtime/Messaging/received.cpp:188`; `B/src/Runtime/Sockets/router.cpp:95`; `B/src/Runtime/Messaging/reply_operations.cpp:16,31,39,78` |
| 공통 benchmark source | Payload 생성·stamp, protobuf encode, JSON header/body native message 생성, submit와 completion 계수다. Binding send에도 caller coroutine slot이 있다. | `P/client/bench_cpp_client.cpp:608,628,673,722` |
| 공통 benchmark target | Protobuf decode·metrics, request이면 protobuf reply encode와 reply submit이다. Binding target은 받은 thread에서 바로 실행한다. | `P/zlink/bench_zlink_cpp_server.cpp:29,55,80` |

따라서 baseline을 “native send 한 번, 할당 0”으로 두면 틀린다. 반대로 binding이 제공하지 않는 logical OperationId, dispatch scope와 application job permit을 native completion과 같은 기능이라고 지워도 틀린다.

## 3. 중복과 결정 소유

| Framework 기구·현재 위치 | Binding의 기존 기능·위치 | 설계에서 남기는 것과 없애는 것 |
|---|---|---|
| Port send result와 pending task, `C/backend/raw_route_port.cpp:190,242` | `send_submission_t.result/admitted`, `B/src/Runtime/Messaging/send_operations.cpp:80,121` | 즉시 result는 값으로 소비한다. Pending의 중간 TCS와 `shared_ptr<task>`를 없애고 기존 observer coroutine이 최종 logical receiver에 연결한다. Binding result는 재구현하지 않는다. |
| Framework pending send, `C/mesh/raw_mesh_node_owner.cpp:1697,1703,1730` | Binding wait-token registry, `B/src/Runtime/Messaging/completion_owner.cpp:608` | Physical retry registry는 binding만 사용한다. S01 §10이 요구하는 logical shutdown·terminal entry는 pending send에만 하나 남긴다. 두 registry는 서로 다른 authority다. |
| Request observer, `C/backend/raw_binding_adapter.hpp:147` | Admitted와 reply awaitable, `B/src/Runtime/Messaging/request_reply.cpp:132` | 이 coroutine을 그대로 유지한다. Admission을 정상 즉시 결과에서도 다시 await하지 않는다. Native retry·drain을 추가하지 않는다. |
| Logical pending·deadline·dispatcher, `C/foundation/operation_registry.cpp:109,382` | Physical completion ID와 timeout, `B/src/Runtime/Messaging/operation_submit.hpp:164` | Full OperationId·ReplyRouteId, resolve 이전부터의 deadline와 shutdown 경쟁은 binding에 없다. C1 atomic take 한 곳과 C2 deadline index 하나를 유지한다. Native timer를 logical timer로 간주하지 않는다. |
| Final public·physical 처리와 중간 결과, `H/contracts/dispatch/request_task.hpp:649,714`; `C/channels/channel_runtime.cpp:1765,1895` | Binding result lifetime와 coroutine await slot, `B/src/Runtime/Messaging/async_operation_state.hpp:126` | 최종 typed receiver, admitted/public 결과의 구별과 늦은 physical 처리 수명은 유지한다. 변환별 TCS·observer owner·closure arena를 없애고 codec별 값 변환만 호출한다. |
| 수신 parts·RID 포장, `C/backend/raw_route_port.cpp:473` | `router.recv(received)`의 native parts, RID/token/generation, `B/src/Runtime/Sockets/router.cpp:31`; `socket.cpp:375` | Canonical queue record가 public `received_t`의 저장소를 보관한다. RID→vector→RID와 parts vector를 소유하는 중간 wrapper를 없앤다. |
| Mailbox claim·batch·settlement, `C/mesh/service_mailbox.cpp:172`; `C/stateful/public_host_runtime.cpp:8123,8557` | Native receive 소유권과 reply token | Native ownership을 재사용한다. Binding에는 application FIFO·ready membership·permit·relocation seal이 없으므로 owner queue는 남긴다. Node/Channel의 prefix claim, shared claim, settlement 배열·remaining과 host batch 재포장은 없앤다. |
| Application job permit, `C/dispatch/application_job_queue.hpp:454,977` | Core physical byte HWM | 중복이 아니다. 기존 batch reserve와 atomic fast return을 그대로 사용한다. Handler 시작 전 job 수를 byte HWM으로 대신하지 않는다(S04 §1·3). |
| Port socket gate·receive poller, `C/backend/raw_route_port.cpp:192,421,475,538` | Native socket call lifetime guard, `B/src/Runtime/Messaging/operation_submit.hpp:39` | Native guard는 재사용한다. C++ socket wrapper와 close의 동시 접근을 보호하는 port gate는 남긴다. Binding 내부 guard만 근거로 wrapper gate까지 삭제하지 않는다. Poller는 현재 하나다. |
| Lane 진입과 port lifecycle 재조회, `C/mesh/raw_mesh_node_owner.cpp:1551,1658,3611` | Binding은 Framework C2 상태를 직렬화하지 않는다. | Logical selection·register/close의 필수 lane은 유지한다. Lane 밖의 mutable snapshot으로 대체하지 않는다. Receive port lease는 회전당 한 번 확보한다. |
| Service header·JSON envelope, `C/protocol/service_wire_codec.cpp:3224`; `C/messaging/envelope_codec.cpp:1232` | Binding은 opaque parts만 전송한다. | 둘 다 Framework wire 계약이다. Service prefix/command는 ingress에서 한 번, application header는 codec에서 한 번 읽는다. Binding recv가 JSON을 검증했다고 가정하지 않는다. |
| Dispatcher와 coroutine 포장, `C/mesh/mesh_record_dispatcher.cpp:32`; `C/channels/route_handler_invoker.cpp:10`; `H/contracts/dispatch/task.hpp:1178` | Received reply context는 closure 없이 보존된다. | Registration이 invoker·codec·reply 경로를 소유한다. 전달만 하는 dispatcher 객체와 메시지별 completion closure를 없앤다. Invoker coroutine은 남기며 frame 안에 결과·대기 등록을 둔다. |
| Scope와 handler resolve, `C/configuration/services.cpp:254,290` | 해당 기능 없음 | Scope와 instance는 dispatch마다 새로 만든다. Constructor 연결·service slot·filter 계획만 registration에 둔다. Scope 저장소의 재사용은 instance 공유가 아니다. |

결정 소유는 physical admission·retry·completion drain·native reply routing은 binding, logical target 선택·service admission·permit·scope·logical terminal은 Framework다. C1 take는 `C/foundation/operation_registry.cpp:129,552`, deadline C2는 `:488,723`, source의 register/close 수락은 `C/mesh/raw_mesh_node_owner.cpp:1551`에 모은다. .NET의 lane 무경합 실행은 `N/Execution/ZLinkStateLane.cs:97`, Java의 terminal take는 `J/internal/service/ZLinkServiceOperationRegistry.java:511`에서 대조했다. Kotlin은 Java runtime을 공유한다. Native와 logical 완료를 한 authority로 합치는 변경은 제안하지 않는다.

대안은 두 가지다. Binding receive thread에서 모든 handler를 직접 실행하면 queue·wake 비용은 줄지만 S02 §13과 S08 §4.4의 infrastructure 독립 진행을 깨뜨린다. 선택한 설계는 **canonical owner queue 하나와 기존 coroutine 실행**을 유지하고, queue 뒤의 claim·batch·dispatcher·closure·독립 task state를 제거한다. Gate 없는 owner의 다음 record가 앞 handler의 terminal을 기다리는 규칙도 제거한다. 새 callback 상태기계는 만들지 않는다.

## 4. 제안한 메시지별 작업

먼저 등록 때 완성할 작업은 transport·reply invoker, declared-type codec 선택 결과, 수신 content-type 선택표, handler constructor·dependency slot·filter 연결이다. 임의 declared type은 첫 사용 때 기존 S05 §7 cache에 넣는다. Topology 변경은 기존 selector 소유자가 처리한다. Current peer의 logical admission과 owner/seal 검증은 메시지마다 유지한다. 불변 생성 계획과 현재 수락 판정을 혼동하지 않는다.

수신은 기존 host permit을 batch로 예약한 후 canonical record의 `received_t`에 바로 받는다. Service header와 route fence를 확인하고 owner queue에 한 번 commit한다. Queue head 하나를 take한 worker가 application header·payload를 읽고, 새 scope·handler를 구성해 기존 coroutine invoker를 시작한다. Record의 소유권이 terminal까지 payload·context·reply token을 유지한다. Prefix claim을 끝내야 다음 Node record가 실행되는 조건은 없다. Spot/Actor의 gate·seal·relocation prefix는 해당 소유자에 남는다.

Request는 최종 typed receiver를 제출 전에 준비하고 logical entry·deadline 참조·dispatcher 자리를 등록한 다음 binding request를 한 번 시작한다. 기존 `observe_request_completion` coroutine이 admitted/reply를 기다린다. Native 알림 자리에서 C1 take를 한 번 하고 각 codec이 값 변환을 한 번 수행한다. Public 결과가 먼저 끝난 경우에도 이미 인도된 physical 결과의 처리·정리를 끝낸다. Application 재개는 독립 completion turn이다.

정상 request의 최종 typed 값은 receiver 안에 한 벌만 보관하고 public·physical 관찰이 그 저장소를 사용한다. 현재 `H/contracts/dispatch/request_task.hpp:696,698`처럼 `projected`를 physical 결과에 복사하고 public 결과로 다시 옮기는 동작은 제거 대상이다. Public 결과가 먼저 실패했다면 그 실패를 유지하고 인도된 physical 값은 변환·관측·정리만 한다. API가 호출자에게 요구하는 값 반환 copy와 내부 결과 보관용 copy는 구별한다. `encoded_payload_t`의 copy는 deep copy일 수 있으므로(`H/contracts/codecs/serializer.hpp:85`) view를 넘길 때 복사 생성자를 호출하지 않는다. 기존 `encoded_payload_from_raw(span)`을 사용하며 coroutine이 참조하는 span의 backing record는 terminal까지 유지한다.

아래 µs는 **설계 가설**이다. 측정치의 신뢰구간이나 보장된 상한이 아니다. 재계산한 stage를 그대로 축소 배분하지 않고, 남기는 연산 수로 추정했다. Instruction 환산은 3,000 cycle/µs·평균 1 instruction/cycle, atomic RMW 15–50 ns, 무경합 mutex lock/unlock 30–80 ns, pool acquire+release 80–160 ns를 가정했다. 이 머신의 측정된 frequency·allocator latency라고 주장하지 않는다. Copy는 16,000 byte/µs 시나리오이며 4096은 1024보다 `3072/16000=0.192 µs`를 service payload 쓰기마다 추가한다. 실제 cache miss·경합·OS 대기는 별도 미확정 항이다.

Header 근거는 `ctrecv`의 raw user ticks와 attribution sample 수로 재계산한 baseline decode 중앙값 0.651292, step-a의 0.070457 µs다. Source service-frame group은 `csrc`의 같은 capture user CPU와 배타 group 비율로 다시 계산하면 change에서 0.372369다. ccall2는 raw caller schedstat와 sample period를 재계산했다. Profile caller 19.077987, 무프로파일 caller 16.612437이며, 후자에 비율을 투영한 encode/register/observe/binding/other는 2.121974/5.755490/2.761472/4.999718/0.973782다. **이 stage는 비용 근거이며 미래 절감량의 합이 아니다.** ccall2의 intrusive task owner는 현재 snapshot에 없으므로 그 profile을 현재 allocation 구조의 측정으로 부르지 않는다.

| ccall2 caller의 배타 leaf | Raw period에서 재계산한 profile µs/request | 설계에 반영한 점 |
|---|---:|---|
| `_int_malloc` / `malloc` self | 0.634264 / 0.350514 | Allocation 호출 수는 이 표본 수로 구할 수 없다. Pool만 도입하고 객체 수를 유지하는 설계는 제외한다. |
| `pthread_mutex_lock` / `unlock` self | 0.550808 / 0.233676 | Binding·Framework·pool이 섞인 값이다. 전부 제거 가능한 Framework mutex 비용으로 빼지 않는다. |
| `allocate_task_owner` 관련 여섯 leaf의 self 합 | 0.333823 | 생성뿐 아니라 destroy/deallocate closure도 포함한다. 현재 코드에 이 owner가 있다고 주장하지 않는다. |
| `make_task_observer` 두 leaf의 self 합 | 0.133529 | 같은 coroutine의 observer owner를 별도 allocation으로 만들지 않는다. |
| `task_control_t::release` self | 0.116838 | Intrusive refcount도 비용이다. Frame 소유권과 결과 소유권의 참조 연산을 count 검사에 포함한다. |

이 표의 비용은 앞 stage에 이미 들어 있다. 합계에 다시 추가하지 않았다. Symbol은 당시 allocation 경로의 근거이고, 현재의 대응 생성 지점은 `H/contracts/dispatch/task.hpp:767,1178`, `H/contracts/dispatch/request_task.hpp:717`이다.

### 4.1 Send source

| ID·남기는 작업 | 요구 조항 | 다른 언어에서 같은 반복 작업을 하지 않는 근거 | 1024 µs·추정 방법 |
|---|---|---|---:|
| S1. Public single-use claim, declared codec plan, call 소유 저장소 한 개 | S01 §2 `:38`; S05 §7 `:189,218` | Java `J/messaging/ZLinkChannelEnvelope.java:510,523`은 단순 header plan을 재사용한다. Public call 소유 자체의 제거 사례는 none found다. | 0.10–0.20. Pool 한 쌍, once RMW 한 번, 고정 type-plan 조회다. |
| S2. Logical target·현재 admission, operation owner turn, port gate | S08 §3 E2·E4 `:97,99`; S06 §5 `:168`; W §4 `:330` | .NET `N/Execution/ZLinkStateLane.cs:97,111`은 무경합 결과용 TCS 없이 caller turn을 실행한다. Logical admission 제거 사례는 none found다. | 0.12–0.30. Lane·port의 짧은 보호 구간과 고정 peer 조회다. 경합 대기는 미포함이다. |
| S3. Canonical application header bytes 재사용·동적 필드 확인 | W §2 `:181,186`; S08 §3 E1 `:96` | .NET `N/Messaging/ZLinkEnvelopeCodec.cs:188,194`, Java `J/messaging/ZLinkChannelEnvelope.java:523`은 단순 header를 재인코딩하지 않는다. | 0.09–0.12. csrc change header 0.088973을 기준으로 비교·bytes 보관 비용을 남긴다. |
| S4. 현재 service payload frame 조립 한 번 | W §2 `:188,197`; S08 E1 `:96` | 같은 wire의 조립 자체를 없앤 사례는 none found다. | 0.30–0.45. csrc의 0.372369를 포함하는 범위다. 기존 writer를 유지하며 body의 wire 필수 쓰기를 0으로 세지 않는다. |
| S5. Binding에 parts 소유권 이전, native 결과의 값 변환, 수명 정리 | S05 §4 `:60,94`; S01 §15 `:569` | Binding이 parts를 소유하며 `.NET`의 prepared dispatch는 `N/Spots/ZLinkMeshNodeRouteDispatcher.cs:247`에서 ready 결과를 직접 소비한다. 동일 C++ 수명 표현의 제거 사례는 none found다. | 0.08–0.16. 고정 part descriptor와 result 값, 제한된 retain/release다. Native submit 자체는 baseline에 있다. |
| S6. 비활성 진단의 enable 확인 | message-flow-tracing §5 `06-observability/03-message-flow-tracing.ko.md:262,276` | .NET `N/Handlers/ZLinkHandlerDispatcher.cs:105`는 Enabled 안에서만 flow 객체를 만든다. | 0.02–0.04. Branch와 context 접근만 남기며 문자열·event·closure 생성은 0이다. |

Known-ready immediate send의 source 합은 **0.71–1.27 µs**다. 현재 immediate 경로에는 원래 coroutine frame이 없으므로 frame을 없앤 절감은 계산하지 않았다(`C/host/app.cpp:2603,2635`; `C/mesh/raw_mesh_node_owner.cpp:1756`). Pending 비율을 `p_s`, completion 통지당 건수를 `K_c`라 하면 logical entry·최종 result·기존 observer coroutine·C1 take·독립 delivery item에 `p_s×[0.80–1.56+(0.8–2.0)/K_c]`를 추가한다. Caller 검토에는 frame 생성이 빠지지 않도록 pending 전체 비용 `p_s×0.80–1.56`을 보수적으로 배정한다. 일부 resume·take는 실제로 completion 자원에서 실행하므로 이 caller 배정은 thread별 실측값이 아니다. Pending의 native retry는 baseline 바깥 Framework 작업으로 다시 세지 않는다.

### 4.2 Receive와 target handler

| ID·남기는 작업 | 요구 조항 | 다른 언어 대조 | µs·추정 방법 |
|---|---|---|---:|
| R1. Service prefix·shape·logical peer fence·owner 분류 한 번 | W §2 `:152`, §4 `:330`; S08 §4.1 I3 `:186` | Binding이 generation을 제공하지만 logical fence 제거 사례는 none found다. .NET도 `N/Spots/ZLinkMeshNodeRouteDispatcher.cs:384`에서 분류한다. | 0.18–0.30. Native receive 위의 고정 field/bounds·peer 조회다. |
| R2. 기존 permit 이전·owner commit·ready 게시 | S04 §3 `:116,235`; S08 I1·I4 `:184,187` | 의미를 없앤 사례는 none found다. | 0.10–0.18. 기존 permit RMW, record slot 취득, batch commit의 분담이다. Record slot 반환은 R9에 센다. |
| R3. FIFO head 하나 take·ready 갱신 | S02 §2 `:97`; S04 §3 `:194` | .NET `N/Spots/ZLinkMeshNodeRouteDispatcher.cs:220,247`은 claim turn 다음의 별도 channel queue를 사용하지 않는다. Java `J/channels/ZLinkHandlerDispatch.java:16`은 여전히 queue/future를 만든다. | 0.10–0.18. 짧은 take lock 한 번과 pointer·membership 갱신이다. Prefix claim·record별 settlement bit는 없다. |
| R4. Outer payload와 inner part의 bounds·native view | W §2 `:207`; S05 §2·4 `:34,70` | .NET의 `ApplicationPayloadView`, `N/Spots/ZLinkMeshNodeRouteDispatcher.cs:234,688`은 part 재포장을 피한다. | 0.08–0.14. 고정 두 part의 길이 검사와 span이다. Payload deep copy는 0이다. |
| R5. Application header 검증, 반복 header hit이면 기존 불변 plan 재사용 | S05 §6 `:132,166`; W §2 `:183` | .NET `N/Messaging/ZLinkEnvelopeCodec.cs:377,1396`은 bytes 일치 hit에서 parser를 호출하지 않는다. | Hit 0.07–0.10. ctrecv sample 재계산값이 근거다. Request/metadata/flow 등 miss는 0.65–1.00으로 대체한다. |
| R6. Handler·수신 codec 선택과 context 구성 | S08 W2·W3 `:221,253`; S05 §7 `:189,218` | .NET `N/Handlers/ZLinkHandlerDispatcher.cs:98,102`와 prepared catalog `N/Handlers/ZLinkScopedHandlerInstanceOwner.cs:95,111`은 생성 절차를 다시 고르지 않는다. | 0.08–0.16. 고정 key 조회와 canonical record를 참조하는 context다. Scoped instance는 cache에 넣지 않는다. |
| R7. 새 scope·handler·dependency 수명 | API §10·11 `:485,504`; S08 §4.3 `:253` | none found. .NET `N/Handlers/ZLinkHandlerDispatcher.cs:96`, Java `J/channels/ZLinkChannelHandlerInvoker.java:443`도 새 실행 객체를 만든다. | 0.18–0.35. 빈 slot 저장소 재사용, 작은 handler 한 개 재구성, 고정 dependency 연결·해제다. 사용자 constructor의 임의 업무 비용은 여기에 보장하지 않는다. |
| R8. 기존 invoker coroutine 한 개 | CPP §3 `:782,788`; S08 W3 `:222` | .NET `ValueTask`의 ready 소비는 `N/Spots/ZLinkMeshNodeRouteDispatcher.cs:251`이다. C++ coroutine 제거의 근거로 사용하지 않는다. | 0.18–0.46. Frame pool 한 쌍 0.08–0.16와 생성·완료·resume 0.10–0.30이다. |
| R9. Terminal 소유권·scope·record 반환 | S05 §4 `:94`; API §10 `:488` | .NET `N/Handlers/ZLinkHandlerDispatcher.cs:131`, Java `J/channels/ZLinkChannelHandlerInvoker.java:450`도 완료 뒤 정리한다. | 0.08–0.16. Move-only record 소유자의 단일 release다. Handler entry permit 반환은 R2의 permit 비용에 포함한다. |
| R10. 비활성 진단 확인 | message-flow-tracing §5 `:262,276` | .NET `N/Handlers/ZLinkHandlerDispatcher.cs:105` | 0.02–0.04. 문자열·event를 만들지 않는다. |
| 회전·통지 분담 | S08 §4.1·4.2·W5 `:183,197,224`; S04 §4 `:305` | .NET `N/Spots/ZLinkMeshNodeRouteDispatcher.cs:220`은 batch를 순회한다. 관리·wake 제거 사례는 none found다. | 관리 0.30–0.80, worker 게시·통지 CPU 0.80–2.00, 기존 owner drain coroutine 0.18–0.46를 각각 실제 K로 나눈다. |

Protobuf decode와 benchmark handler/metrics는 binding에도 있으므로 추가 합계에 넣지 않는다. Request reply의 protobuf encode도 같다. 0으로 비용을 주장하는 것이 아니라 §1의 binding CPU에 남기는 것이다. 새 scope·handler는 이 공통 기준에 없으므로 R7에 추가했다.

Queue와 ready membership은 현재 mailbox의 scheduling 소유자가 하나의 원자 구간에서 결정한다. Executor의 별도 ready 사본은 두지 않는다. Worker 수·상한·W5 시간 예산은 유지하고, record의 enqueue/take마다 무조건 wake하지 않는다. 통지는 S04 §3의 publication/idle 경계에서 기존 예약 자원에 합친다. Static cheap batch 때문에 worker를 모두 깨우는 규칙이나 cdrain2의 backlog 비교·helper 탐색을 추가하지 않는다. 동시에 실행 가능한 worker가 여러 개면 동일 Node의 서로 다른 record를 처리할 수 있어야 한다. K는 그 결과 관측되는 통지당 건수이며 64를 가정하거나 batch가 찰 때까지 기다리지 않는다. §4.4의 receive-role handoff가 가능하면 쓰되, 이 설계의 절감 근거에는 성공을 가정하지 않았다.

구체적으로 `C/mesh/mesh_node_host_service.cpp:3356`의 owner별 callable 게시를 제거한다. Host 등록 때 executor에 mailbox drain source를 한 번 연결하고, worker는 그 source의 canonical ready 집합에서 record를 take한다. Executor에는 ready owner를 다시 enqueue하지 않는다. 기존 executor의 다른 application 작업 queue는 그대로 자기 작업만 소유한다. Worker의 idle 등록·재확인과 publication 통지 예약은 기존 scheduler mutex가 담당하고, 실제 record commit/take는 mailbox mutex가 담당한다. Publisher는 mailbox mutex를 놓은 뒤 scheduler를 알리며 handler는 두 보호 구간 밖에서 실행한다. 등록된 source의 선택·cursor 순회는 worker 회전당 비용이다. 이 benchmark는 source 하나지만 여러 mailbox의 선택 비용을 무료로 가정하지 않고 회전 비용에 포함해 계수해야 한다. 이는 새 per-record queue 대신 기존 owner callable·ready 통지 목록을 제거하는 private 실행 경계 변경이다.

### 4.3 Request source와 reply completion

| ID·남기는 작업 | 요구 조항 | 다른 언어 대조 | 1024 µs·추정 방법 |
|---|---|---|---:|
| Q1. Single-use·codec plan·시작 deadline | S01 §2·9 `:38,357`; S05 §7 `:218` | Java `J/internal/service/ZLinkServiceOperationRegistry.java:216`도 절대 deadline을 만든다. 제거 사례는 none found다. | 0.10–0.20. Clock·고정 조회·claim이며 저장소 할당은 Q6다. |
| Q2. Logical resolve/admission·owner turn·port gate | S08 §3 E2–E4 `:97` | .NET `N/Execution/ZLinkStateLane.cs:97`의 무경합 caller turn을 참고한다. | 0.12–0.30. S2와 같은 연산이다. |
| Q3. 동적 request header의 canonical encode | W §2 `:181`; S01 §9 `:357` | .NET `N/Messaging/ZLinkEnvelopeCodec.cs:199,211`도 correlated header는 단순 cache에서 제외한다. 제거 사례는 none found다. | 0.35–0.60. 준비된 type/channel 조각과 correlation·deadline 쓰기에 1,050–1,800 cycle을 가정했다. 날짜 형식·검증을 생략하지 않는다. |
| Q4. Service payload 조립 | W §2 `:188`; S08 E1 `:96` | none found | 0.30–0.45. S4와 같다. |
| Q5. Full logical identity·pending entry·deadline index·dispatcher 자리 등록 | S01 §10·10.1 `:392,424,447`; S08 E3 `:98` | Java `J/internal/service/ZLinkServiceOperationRegistry.java:328,342,511`도 logical entry를 가진다. Binding entry만 쓰는 사례는 none found다. | 0.30–0.55. 작은 pending node pool, C1 publication lock, deadline link, 고정 cancellation 연결이다. C1 take는 C3다. |
| Q6. Fluent call과 최종 typed receiver 저장소 | S01 §9 `:331,333`; §2 admitted/reply `:68` | .NET은 언어 Task를 쓴다. C++ call·receiver의 수명 자체를 제거한 동등 사례는 none found다. | 0.20–0.35. 수명이 다른 call과 최종 receiver 두 block이다. Public/admitted/인도된 physical 처리의 구별은 유지하되 변환별 결과 block은 0이다. |
| Q7. 기존 binding observer coroutine | S08 E4·E5 `:99,100`; CPP §3 `:788` | none found. Binding awaitable을 직접 기다리는 coroutine을 보존한다. | 0.18–0.46. Frame 한 개, 정상 admission 뒤 reply suspension/resume 한 번이다. |
| Q8. Codec 경계의 값 변환·소유권 이전 | S01 §9 `:331`; S05 §6 `:135` | .NET의 raw view 전달은 `N/Spots/ZLinkMeshNodeRouteDispatcher.cs:688`이다. | 0.08–0.16. 별도 task·observer 없이 기존 최종 receiver의 데이터로 전달한다. |
| Q9. 비활성 진단 확인 | message-flow-tracing §5 `:262` | S6와 같다. | 0.02–0.04 |
| C1. Reply service view와 JSON header decode | W §2 `:152,207`; S05 §6 `:135` | .NET `N/Messaging/ZLinkEnvelopeCodec.cs:399`도 correlated reply cache hit를 가정하지 않는다. | Header 0.65–1.00, view 0.08–0.14. Typed protobuf decode는 binding 공통 비용이다. |
| C3. Logical terminal take·stop 연결 해제 | S01 §10·10.1 `:392,462` | Java `J/internal/service/ZLinkServiceOperationRegistry.java:516`의 pending remove가 같은 authority다. | 0.10–0.20. C1 한 번이며 deadline lane 왕복은 0이다. |
| C4. 최종 결과 publish와 물리 자원 정리 | S01 §9 `:333`; §11 `:519` | none found | 0.10–0.20. Public 결과를 재완료하지 않고 physical 정리를 끝낸다. |
| C5. 독립 dispatcher item·application 재개 | S01 §11 `:498,508,525`; S08 E5 `:100` | .NET #3995는 target의 두 번째 queue를 없앴다. Source 새 completion turn까지 없앤 사례는 none found다. | Item 0.10–0.20와 `(0.80–2.00)/K_c` 통지 CPU다. Callback 하나의 반환에 다른 callback을 의존시키는 drain으로 합치지 않는다. |

Request submit 합계는 **1.65–3.11 µs**, completion은 **1.03–1.74+(0.8–2.0)/K_c**다. Admission pending이면 같은 observer frame의 추가 suspend/resume·admitted 전달에 `p_r×0.10–0.20`을 더한다. Timer 객체, 재제출 queue와 중간 completion task는 만들지 않는다. Owner lane 경합 대기는 이 범위에 들어 있지 않다.

Target request는 receive의 R5 hit를 0.65–1.00의 dynamic decode로 바꾸고, **기존 typed reply adapter coroutine 한 개 0.18–0.46**와 reply header 0.35–0.60, service 조립 0.30–0.45를 추가한다. 따라서 **2.48–4.48+(1.28–3.26)/K**다. Reply는 handler의 완료 turn에서 binding reply operation을 시작한다(S08 W4, `:223`). 다른 executor에 reply 작업을 게시하지 않는다.

### 4.4 Coroutine의 개수·크기·수명

공통 invoker를 ready이면 일반 함수로 바꾸는 ctrecv 단계(b)는 **반복하지 않는다**. Public awaitable, user handler coroutine, internal observer·invoker·typed adapter·owner drain coroutine을 보존한다. Frame 수를 줄이는 nested layer 병합은 아래 질문에만 두며 합계에서 빼지 않았다.

| Coroutine | 메시지당 frame 수 | 설계 byte 상한·현재 확인 범위 | 할당·resume 추정 |
|---|---:|---|---|
| Known-ready send source | 0, pending일 때 기존 observer 1 | Pending observer ≤512 byte를 제안한다. 현재 정확한 compiler frame 크기는 미제공이다. | Immediate에서 절감 0. Pending은 0.18–0.46 µs를 pending 추가식에 포함했다. |
| Request source observer | 1 | ≤512 byte 제안. `C/backend/raw_binding_adapter.hpp:147,167,186` | Frame pool 0.08–0.16 + 생성·재개·완료 0.10–0.30 = 0.18–0.46 µs다. |
| Target invoker | 1 | ≤2048 byte 제안. `C/channels/route_handler_invoker.cpp:10` | 같은 0.18–0.46 µs다. Frame 크기만큼 memset하지 않으며 실제 작성 cache line 수가 상한을 넘으면 재추정한다. |
| Target typed request adapter | 1 | ≤512 byte 제안. 동기 echo도 `H/contracts/configuration/mesh_node.hpp:643,684`가 coroutine이다. | 0.18–0.46 µs를 target request에 별도로 포함했다. |
| Owner drain | 회전당 1, 메시지당 1/K | ≤512 byte 제안. `C/mesh/mesh_node_host_service.cpp:3861` | `(0.18–0.46)/K`다. Receive/관리 장기 coroutine의 고정 frame은 연결·host 수명에 귀속한다. |
| User async handler·nonempty filter | Benchmark에서는 0 | 두 benchmark handler는 동기다(`P/framework/bench_framework_cpp_server.cpp:23,34`). 일반 handler frame 크기·filter 수에는 이 여섯 셀의 고정 상한을 적용하지 않는다. | 해당 coroutine을 유지하며 workload별로 추가한다. Filter의 기존 두 nested coroutine은 `C/handlers/handler_registry.cpp:86,121`이다. |

Byte 상한은 측정값이 아니며 **구현 전 컴파일 결과로 확인해야 하는 설계 조건**이다. 현재 `envelope_header_t` 자체는 576 byte다(`C/messaging/envelope_codec.cpp:378`). 이 객체와 context의 사본을 frame 여러 개에 넣고 위 상한을 주장할 수 없다. Canonical record의 소유 context를 참조하고 그 record를 suspension 동안 유지해야 한다. Framework가 프레임을 없애서 상한을 맞추는 것은 허용하지 않는다.

Coroutine 생성·완료·resume 0.10–0.30의 계산은 frame의 실행/결과 수명과 첫 대기 등록에 RMW 3–5회, 나머지 pointer·상태 처리에 단순 instruction 약 150개를 가정한 것이다. `3×0.015+150/3000≈0.10`, `5×0.050+150/3000=0.30 µs`이며 pool 0.08–0.16을 별도로 더했다. Refcount를 intrinsic 또는 intrusive로 표현해도 이 비용은 남는다. Coroutine frame 소유권에 직접 속한 RMW가 정상 한 번의 대기에서 5회를 넘으면 이 모델과 count 검사를 실패시킨다. Admission pending의 추가 대기는 Q7의 pending 추가식에 계수한다. Public 결과·registry·binding 내부의 원자 연산을 이 frame 수에 섞거나 전체 경로에서 제외하지 않는다.

Allocator-aware promise가 frame과 그 결과·첫 await 등록을 같은 block에 둔다. 별도 `make_shared<coroutine_task_shared_state_t>`와 observer owner block을 제거한다(`H/contracts/dispatch/task.hpp:767,1178`). 생성·실행 중 참조와 결과 handle 참조를 구별하되 수명 제어 block은 하나다. Final suspend에서 결과를 게시한 뒤 실행 참조를 반납하고, 마지막 handle까지 frame을 보관해야 하면 callback·payload·scope capture는 terminal에서 먼저 정리한다. `suspend_never`로 frame을 파괴하면서 그 내부 result를 public handle에 빌려 주는 설계는 불가다. Binding observer는 fire-and-forget coroutine으로 유지하며 public 결과가 먼저 끝나도 native drain까지 frame을 반환하지 않는다.

Pool은 크기별 재사용 저장소일 뿐 admission authority가 아니다. Cross-thread free가 가능한 synchronized resource를 사용하고 그 lock을 위 비용에 포함한다. Remote-return queue, 추가 scheduler hop, 고정 4096-byte callback arena와 별도 intrusive callback owner는 추가하지 않는다. Cold/growth allocation은 허용·계수하며, warmed 상태의 system heap allocation만 0을 목표로 한다. Pool 호출과 frame byte 수를 0으로 표시하지 않는다. 이는 ccall2의 owner 교체만으로 절감을 주장하는 접근과 다르다.

질문으로만 남기는 병합 후보는 typed request adapter를 invoker coroutine 안으로 합치는 것이다. 최대 후보 비용은 0.18–0.46 µs, source 쪽 기존 전달 전용 nested coroutine이 실제로 남은 경로도 frame마다 같은 범위다. **위 합계는 이 절감을 포함하지 않는다.** Callback이나 수동 상태기계로 대체하는 후보는 없다.

## 5. 셀별 합계와 판정

`K`는 실제 worker 통지당 처리 건수, `K_c`는 completion 통지당 건수다. 수신 한도 64와 평균 batch 크기는 다르다. 아래 포화 셀은 **K=K_c=8, p_s=p_r=0인 검토 시나리오**이며 실제 입력에서 이 값이 관측됐다는 뜻이 아니다. Serial은 K=K_c=1, p_r=1도 포함해 계산했다. 모든 값은 binding 위의 추가 user+kernel CPU 가설이다. 표의 Framework 몫은 §1의 두 목표를 함께 적용한 값이다.

| 셀 | Caller 추가 합계 / 허용량 | Source 추가 CPU / 허용량 | Target 추가 CPU / 허용량 | 판정 |
|---|---|---|---|---|
| send 1024 | 0.710–1.270 / 1.431 | 0.710–1.270 / 1.955 | 1.230–2.478 / 1.849 | Target 상단이 0.628 초과한다. 예산 미충족 범위가 있다. |
| send 4096 | 0.902–1.462 / 4.051 | 0.902–1.462 / 3.627 | 1.230–2.478 / 3.618 | 이 시나리오의 CPU 합계는 들어간다. 실제 I/O thread 상한은 미확정이다. |
| backpressure 1024 | 1.650–3.110 / 2.676 | 2.780–5.100 / 6.377 | 2.640–4.888 / 6.251 | Caller 상단이 0.434 초과한다. Process CPU만 통과해도 부족하다. |
| backpressure 4096 | 1.842–3.302 / 4.517 | 2.972–5.292 / 11.259 | 2.832–5.080 / 11.698 | 이 시나리오의 CPU 합계는 들어간다. |
| serial 1024 | 1.750–3.310 / 87.670 | 3.580–7.050 / 30.031 | 3.760–7.740 / 24.039 | CPU는 들어간다. 왕복 대기를 별도로 제한해야 한다. |
| serial 4096 | 1.942–3.502 / 98.037 | 3.772–7.242 / 29.233 | 3.952–7.932 / 24.918 | 절대 목표의 CPU 예산에도 들어간다. 왕복 대기는 미확정이다. |

조건을 바꾸면 다음과 같다.

| 조건 | 다시 계산한 영향 |
|---|---|
| Send 1024, K=64·p_s=0 | Target 상단도 2.120938로 허용량 1.849425보다 0.271512 크다. Full batch만 가정해도 상단 예산이 닫히지 않는다. |
| Send 1024, K=8·p_s=1 | Source 전체 추가 1.610–3.080, caller 검토 배정 1.510–2.830이다. 두 상단 모두 초과한다. Caller 조건은 `1.27+1.56p_s≤1.431373`, 즉 이 상단 모델에서 `p_s≤0.103444`다. |
| Backpressure 1024, p_r=1 | Caller 상단 3.310, 허용량 초과 0.633963이다. Native admission을 다시 구현해서 이 비용을 없애는 방식은 제외한다. |
| Backpressure 4096, p_r=1·K=8 | Caller 3.502, source 5.492, target 5.0795로 이 CPU 모델 안에는 들어간다. |
| Serial 1024 | 추가 endpoint CPU를 모두 직렬 비용으로 더하는 시나리오는 7.340–14.790이다. Binding RTT에 상단을 더하면 76.735618 µs다. 남은 **26.507078 µs** 안에 추가 lane·scheduler 대기와 native 비용 증가가 모두 들어가야 103.242696을 만족한다. |
| Serial 4096 | 같은 계산은 7.724–15.174, 총 86.503140 µs다. 남은 **28.505486 µs**가 115.008626까지의 추가 대기·native 비용 허용량이다. |

원시 profile에는 K, p_s, p_r의 완료된 전수 count가 없다. Serial의 C++ 구간별 scheduler 지연도 없다. .NET #3995의 약 10.8 µs handoff를 C++의 실측값으로 대입하지 않았다. 이 세 미확정 값을 0으로 놓고 여섯 셀 합격을 선언하지 않는다.

포화 처리량은 thread별 service demand의 최댓값으로 제한된다. 제안 경로의 ingress 추가 상단은 대략 `R1+R2+회전분담=0.48+3.26/K`, worker는 R3–R10과 그 자원의 통지 비용이다. 같은 비용을 process 합계에 두 번 넣지 않고, thread 판정에는 어느 thread에 실행되는지를 계수해야 한다. Target의 binding receive+decode 전체를 ingress와 worker 양쪽에 한 번씩 더하지 않는다. Send 4096의 미제공 β_IO·β_receive 때문에 이 계산을 정확한 처리량 예측으로 끝낼 수 없다.

또한 `Framework_CPU = binding_CPU + 위 합계 + Δ_native`다. Δ_native는 wire 크기·submit 간격·batching·kernel 경로가 달라져 생기는 하위 계층 CPU 차이다. csrc/ctrecv raw에서 실제 차이가 있으므로 0으로 확정할 수 없다. 이번에는 새 측정을 금지했으므로 그 증가를 없앤 절감량도 인정하지 않았다. 표의 양수 여유는 이 미확정 비용까지 수용해야 한다.

## 6. 비용이 남는 조항과 결정 필요 사항

다음은 **상단 합계가 예산을 넘을 때 검토할 조항**이다. 추정 상단의 초과가 “이 spec으로 달성 불가능”이라는 수학적 하한 증명은 아니다. 먼저 개수·할당·경합을 줄인 설계가 실현 가능한지 PM이 검토해야 한다. Coroutine 제거는 선택지에서 제외했다.

| 조항·남는 요구 | 메시지당 비용 가설 | 조항이 달라지면 가능한 설계·대가 |
|---|---|---|
| W §2 `:188,197`, service multipart envelope | Source마다 S4/Q4 0.30–0.45, 4096은 +0.192다. Request reply도 같은 비용이다. | Payload parts를 binding multipart로 직접 보내면 outer 조립을 제거할 수 있다. Schema와 네 언어 wire가 바뀌므로 C++ 전용 변경은 불가다. 현재 합계는 이 변경을 사용하지 않았다. |
| API §10·11 `:485,504`, S08 §4.3 `:253`, dispatch별 scope·instance | R7 0.18–0.35다. K=8 send 상단 초과 0.628의 대부분과 비슷하지만 이것 하나를 모두 빼도 상단 여유는 부족하다. | Handler·dependency의 process 재사용을 허용하면 scope·재생성 비용 일부를 제거할 수 있다. Scoped isolation과 dispose 의미가 바뀐다. 빈 저장소 재사용은 조항 변경 없이 먼저 할 수 있다. |
| S04 §3 `:116,235`, owner queue와 permit | R2+R3 0.20–0.36, 회전 permit 예약은 관리 분담 안에 있다. | Handler 시작 전 job capacity·owner FIFO를 없애야 binding recv 직후 실행으로 완전히 제거할 수 있다. Physical byte HWM은 같은 보호가 아니므로 단순 대체할 수 없다. 제안하지 않는다. |
| S02 §13 `:495`, S08 §4.4 `:306`, 수신·application 독립 진행 | Worker 게시·wake CPU `(0.8–2.0)/K`, serial은 그대로 0.8–2.0이며 wall 대기는 미확정이다. | Receive thread에서 handler를 실행하면 handoff를 제거할 수 있으나 긴 동기 handler가 receive·completion·관리를 막는다. 현재 계약의 역할 인계 성공 때만 같은 자원 실행이 가능하다. 그 성공을 가정한 savings는 없다. |
| S01 §10·10.1 `:424,447`, logical entry·identity·deadline 등록 | Q5 0.30–0.55다. Request 1024 caller 초과는 p_r=0에서 0.434, p_r=1에서 0.634다. | Binding completion만 남기는 설계는 resolve 이전 deadline·logical shutdown·relocation identity 처리를 잃는다. 해당 의미를 binding 계약으로 내려 보내거나 제거해야 하므로 Framework 내부 최적화로 승인할 수 없다. |
| S01 §9 `:333`, admitted/public 완료 후 physical 처리 | Q6 중 최종 receiver와 C4 0.10–0.20, pending admission 추가 0.10–0.20다. Q6 전체 0.20–0.35를 모두 이 조항의 비용이라고 세지 않는다. | Admission 실패와 동시에 physical 관찰을 폐기하면 보관·변환은 줄지만 이미 인도된 결과 처리와 route-cache 실패 관찰 계약이 달라진다. Binding이 폐기한 late native reply는 원래 이 비용 대상이 아니다. |
| S01 §11 `:498,508`, 독립 새 completion turn | C5 0.10–0.20 + 통지 `(0.8–2.0)/K_c`다. | Binding 알림 stack에서 application continuation을 inline 실행하면 이 비용 일부를 없애지만 재진입·infrastructure 독립·callback 독립 계약이 바뀐다. Callback들을 한 serial drain으로 합치는 방식도 제외한다. |

첫 결정은 **합격 기준 0.60과 현재 S08의 0.90 중 이번 작업의 판정 범위**다. 예를 들어 send 1024의 0.90 process Framework 몫은 source 0.325859, target 0.308238 µs에 불과하여 이 모델은 그 기준을 충족하지 않는다. Spec을 0.60으로 고치자는 제안은 하지 않는다.

두 번째는 **현재 C++ handler 수명과 공통 계약의 불일치**다. `P/framework/bench_framework_cpp_server.cpp:59,61`은 handler singleton instance를 등록한다. `H/contracts/configuration/mesh_node.hpp:60`은 이미 등록된 type이면 새 registrar를 추가하지 않고, `:581,649`는 scope에서 `get_required`를 호출한다. `C/configuration/services.cpp:299`는 singleton을 그대로 돌려준다. 반면 API `:485`는 application DI lifetime이 dispatch별 handler 수명을 바꾸지 못한다고 명시하고 .NET `N/Handlers/ZLinkHandlerDispatcher.cs:96,102`, Java `J/channels/ZLinkChannelHandlerInvoker.java:443`은 새 실행 객체를 만든다. 이는 C++의 기존 계약 불일치(B) 후보이며 singleton 재사용의 비용을 설계의 정상 기준으로 인정하지 않았다. 명시적으로 제공한 singleton instance에는 fresh 생성 factory가 없을 수 있으므로 **기존 C++ 등록 계약으로 생성 계획을 얻을 수 있는지 PM이 먼저 판정해야 한다**. 새 public factory API가 필요하면 설계 결정으로 분리한다. Benchmark의 singleton 조건을 조용히 바꾸거나 scope 비용을 삭제하지 않는다.

분류는 binding 재사용·중복 표현 제거가 B, 공통 execution 계약에 맞춘 record별 take가 A다. 위 표의 wire·수명·progress·completion 의미 변경은 D이며 대표님 결정 전 구현 대상이 아니다. C로 분류되는 retry 증가·poller 추가·caller의 하위 계층 보상은 제안하지 않았다. PM의 report.md 전행 검토와 필요한 대표님 결정 전에는 다음 절의 구현을 시작하지 않는다.

## 6b. 이전 방향과의 차이

`cdrain`과 `cdrain2`는 제공된 보고서의 회차별 숫자에서 중앙값과 비율을 다시 계산했다. `csendb`와 `csasm`은 입력에 원시 run JSON이 없으므로 보고서의 표시값으로만 산술을 확인했다. 그 정밀도 이상으로 재검증했다고 주장하지 않는다. #3995에는 merge 후 처리량 측정이 없다.

| 이전 작업 | 바꾼 것 | 입력에서 재계산한 결과 | 미달 이유·확인 한계 | 이 설계가 반복하는가·차이 |
|---|---|---|---|---|
| cdrain | Gate 없는 Node의 독립 record take와 여러 worker drain | Send 175,407.7→155,334.2 msg/s, −11.4439%. F÷B 0.300576→0.257221. Request −1.8474%. Spot p99 중앙값 501.754→7,585.620 µs다. | 동시 실행은 늘었지만 cheap-message CPU·wake·contention이 늘었다. 개별 원인의 µs까지 분리된 실험은 아니다. | 독립 record take라는 계약은 같다. Worker 확대를 절감 수단으로 쓰지 않고 claim/batch/closure 수 자체를 없앤다. 동시성 통과를 성능 통과로 세지 않는다. |
| cdrain2 | Post-take backlog 비교와 helper rotation으로 추가 wake 축소 | Send 176,977.1→154,844.2, −12.5061%. F÷B 0.284001→0.249088. Spot p99 489.922→7,538.543 µs다. | Cheap static batch wake test 통과만으로 실제 지속 backlog의 worker 수·cache 비용을 제한하지 못했다. | Backlog 비교 상태·active helper 검색을 추가하지 않는다. 기존 worker 상한·W5와 publication/idle 예약 규칙을 사용한다. 실제 K·wake·handoff·Spot 지연을 별도 실패 조건으로 두며, 회귀가 없다고 미리 보장하지 않는다. |
| csendb | 독립 Node claim과 source 최종 service payload 직접 조립 | Send 1024 F÷B는 표시 rate로 0.283879→0.273698이다. 같은-rate source+target CPU 표시 중앙값은 22.051→45.123 µs다. | Target scheduling과 source 조립을 함께 바꿔 원인을 분리할 수 없고, 구조를 바꿨다는 사실만으로 비용이 줄지 않았다. | 직접 조립 encoder를 send에 새로 연결하지 않는다. Source는 기존 writer를 비용으로 남긴다. Queue 수명·task 저장소·binding 결과 전달을 먼저 하나로 정하고 각 변경의 계수를 검사한다. |
| csasm | csendb에서 source 조립만 분리 | 표시 wall time 5.585→5.708 µs로 약 2.20% 증가다. Source CPU 9.071→9.863, +0.792 µs이고 두 process 합은 +1.360 µs다. | Header buffer 하나를 줄여도 deferred encoder·표현·관리 비용이 상쇄했다. 단일 원인을 더 세밀하게 분리할 자료는 없다. | 같은 deferred encoder 실험을 반복하지 않는다. S4에 0.30–0.45를 지불하며 wire 변경은 §6의 별도 결정으로만 둔다. |
| .NET #3995 | Mailbox claim 다음의 두 번째 ChannelName application queue 제거 | PR 본문은 receive→handler thread change 2→1과 사전 handoff 약 10.8 µs를 적는다. 사후 처리량·CPU 원시값은 없다. `ratio054net`은 이후 별도 세션의 request table이지 이 PR의 A/B 측정이 아니다. | C++ 실패 실험으로 묶을 수 없다. 이 변경만으로 전체 budget 충족을 증명한 자료가 없다는 한계다. | 중복 queue를 없앤다는 원칙은 재사용한다. C++에 똑같은 두 번째 channel queue가 있다고 가정하지 않고, 실제 claim·host batch·dispatcher·task state의 소유 수를 줄인다. Source·reply도 함께 계산한다. |

추가로 csrc의 helper·header·parts 최적화와 ctrecv의 header cache·claim coallocation은 같은 영역을 다룬다. Cache hit의 재사용은 유효한 근거로 인정하지만 **frame 1→0은 채택하지 않는다**. 이 설계는 현 단계들을 유지한 채 예산만 다시 나눈 것이 아니라 Node 메시지에 prefix claim·host batch·메시지별 dispatcher·별도 coroutine result block이 필요한지를 삭제 대상으로 정했다. 그래도 §5의 미달과 미확정 항을 남겼다.

## 7. 승인 뒤 구현 순서와 개수 검사

아래는 구현 계획이며 이번 작업에서 수행하지 않았다. µs는 가설이다. 서로 겹치는 stage·allocator·mutex 비용을 독립 절감으로 합산하지 않는다. 먼저 §5의 1 KiB 미달과 §6의 기준·handler 수명 문제를 PM이 검토하고 대표님 결정이 필요한 항목을 확정해야 한다. 결정이 없으면 spec 의미 변경을 구현하지 않는다.

| 순서 | 변경·예상 제거 비용 | PR에서 필요한 검사 |
|---|---|---|
| 1 | Canonical binding receive record와 typed RID/token 수명으로 통합한다. Port lease·service 분류는 회전당/record당 각 한 곳이다. Target 0.2–0.5 µs 후보이며 body deep copy 절감은 현재 move 경로에 없다고 세지 않는다. | Record별 native body copy 0, service classification 1, port lease 1/회전, reply token 원본 유지. Caller wrapper 파괴·close·malformed 입력의 양방향 수명 검사다. |
| 2 | Node/Channel prefix claim·settlement 배열·host batch 재포장을 제거하고 queue head의 소유권을 직접 이전한다. Target 0.8–1.4 µs 후보다. | Prefix claim allocation 0, settlement RMW 0, queue commit/take 각 1. 동일 Node의 A 동기 handler를 붙잡아도 B가 다른 worker에서 진행하며 FIFO take·last-take/enqueue·shutdown을 검사한다. Spot/Actor gate 음성 대조를 유지한다. |
| 3 | Registration에 route/reply/codec/DI 생성 계획을 두고 per-record dispatcher·completion std::function을 제거한다. Target 0.5–1.0, source 0.2–0.5 µs 후보다. | Per-record dispatcher 객체 0, 내부 type-erased closure heap allocation 0, header/handler resolve 각 1 이하, fresh scope·handler·scoped dependency identity를 검사한다. 등록한 invoker 자체는 한 개다. |
| 4 | Coroutine promise의 frame·결과·첫 await 등록을 coallocate/recycle한다. 모든 coroutine layer를 유지한다. Frame마다 0.08–0.20 µs 후보이며 실제 frame 수를 곱해 평가한다. | 아래 frame 수·byte 상한, 시스템 heap과 pool 호출을 따로 계수한다. Coroutine 하나를 callback으로 치환해 통과시키지 않는다. Suspend 뒤 payload/context, early public failure 뒤 physical 완료, retained result handle과 cross-thread destruction을 검사한다. |
| 5 | Request의 codec별 값 변환은 유지하고 최종 receiver만 연결한다. 중간 task state·observer owner·closure arena를 제거한다. ccall2의 등록+관찰 8.516962 µs는 전체 상한 근거일 뿐이며 예상 제거는 caller 1.0–2.0 µs 후보로 제한한다. | Logical pending 1, C1 take 1, C2 deadline index 1, 변환용 비동기 result 0, native operation 1. Reply/admission/timeout/cancel/shutdown 경쟁과 physical 정리 count를 양방향으로 검사한다. |
| 6 | 반복 header와 codec plan을 소유 codec 안에서 재사용한다. Header hit는 과거 raw에서 약 0.58 µs 감소 근거가 있다. Request dynamic header에는 이 절감을 적용하지 않는다. | Byte 일치 hit에서 parse 0, miss parse 1, 잘못된 JSON·content-type·metadata·correlation·flow는 기존 오류다. Cache에 scope·payload·source lifecycle state를 보관하지 않는다. |
| 7 | 기존 scheduler에 batch publication·idle wake 예약을 연결한다. 별도 worker 확대 정책은 넣지 않는다. 절감은 고정값이 아니라 `(1.28–3.26)×(1/K_before−1/K_after)`다. | Record dispatch task 0, receive→handler 최대 1 handoff, reply submit handoff 0, completion 최대 1. K와 p를 실계수한다. 독립 callback을 붙잡아도 다른 callback과 infrastructure가 진행해야 한다. |
| 8 | 독립 검토 뒤 최종 6개 셀을 한 번 검증한다. 이 단계의 예상 절감은 0이며 앞선 합계를 확인한다. | Binding·Framework를 같은 package·조건으로 측정하고 thread CPU, source/target user+kernel, RTT와 allocation/lock/handoff count를 함께 판정한다. 다른 언어 절대값은 새 같은 세션 자료가 없으면 이전 세션이라는 표기를 유지한다. |

정상 warmed 6-cell 경로의 **Framework 추가 개수 상한**은 다음과 같이 제안한다. Binding 내부 수와 protobuf·application의 할당은 별도 baseline으로 계수하며 숨기지 않는다. Pool round trip은 acquire와 release를 합친 한 건이며 synchronized resource lock 두 쌍을 예산에 포함한다. Cold/growth 경로의 실제 system heap bytes도 별도로 기록한다.

| 계수 | Send source 즉시 / pending | Request source | Target send / request |
|---|---|---|---|
| 메시지별 coroutine frame | 0 / 1 | observer 1 | invoker 1 / invoker+typed adapter 2 |
| Batch coroutine frame | completion에 추가 없음 | completion에 추가 없음 | 기존 owner drain 1/회전 |
| Frame 이외 소유 block | call 1 / call+logical pending+final result 3 | call+final receiver+logical pending 3 | canonical record+fresh scope/handler storage 2 |
| Pool round trip | 1 / 4 | 4 | 3 / 4, drain 1/회전 추가 |
| 변환 전용 TCS·observer owner·per-record std::function heap | 0 / 0 | 0 | 0 / 0 |
| Logical terminal authority | 즉시 0 / pending entry의 C1 1 | entry의 C1 1 | Record 소유권 release 1. Source pending과 다른 권한이다. |
| System heap allocation | Warmed 고정 high-water에서 0, native/codec buffer는 별도 | 같음 | 같음 |
| Framework mutex lock/unlock 쌍 | Pool 2 + lane/port ≤3 = ≤5. Pending은 pool·C1·dispatcher 항목을 별도 추가 계수한다. | Pool 8 + lane/port ≤3 + C1 등록/take 2 + dispatcher enqueue 1 = ≤14, 통지당 보호 구간은 별도다. | Pool 6/8 + head take 1 = ≤7/9. Commit·permit reserve·scheduler 보호는 회전당 합계 ≤4이며 release_credits의 slow path는 별도 계수한다. |
| 실행 자원 handoff | 필수 소유 turn 최대 2. Known-ready node는 같은 caller의 turn이면 0이다. | 같음. Completion→application 최대 1 추가 | Receive→handler 최대 1, reply→binding 0 |

이 lock 수는 **의미상 동기화 지점의 상한 제안**이다. `std::pmr` 내부 실제 lock 호출 수는 구현별로 다를 수 있으므로 계측이 이를 넘으면 “pool 내부니까 제외”하지 않고 모델을 실패시킨다. Permit 반환은 현재 `C/dispatch/application_job_queue.hpp:977`의 atomic fast path를 사용한다. Waiter·pressure 전환이 있으면 그 slow path 비용을 0으로 세지 않는다. W5·scope·handler await가 추가한 실제 count도 test에서 포함한다.

Native/codec buffer를 “별도”로 센다는 것은 제외한다는 뜻이 아니다. Source protobuf 출력은 `extensions/framework-codec-protobuf/include/zlink/codecs/protobuf.hpp:27`의 buffer 한 개이고, 현재 send는 JSON header와 최종 service payload를 추가로 만들며 command header도 만든다. **Native message buffer 생성 요청 상한은 send source 4개**로 둔다. Request는 이미 있는 deferred application-header 경로를 유지하므로 protobuf·최종 service payload·command header **3개**다. Target reply는 그 현재 경로의 JSON header·protobuf·service payload·command header **최대 4개**로 둔다. 작은 command header가 native inline storage를 쓰는지는 binding/Core 계수로 확인하고 heap allocation 0이라고 추측하지 않는다. Binding 벤치의 source header/body 두 buffer 생성과 나란히 보고한다. Receive의 추가 body buffer 생성·encoded body deep copy는 0, 필요한 inner part view는 canonical storage에서 빌린다. Source/reply의 wire 필수 body 쓰기는 각각 1회이며 S4/Q4 비용에 포함한다. 각 buffer의 count·bytes·native heap 호출을 이 상한과 함께 검사한다.

각 PR의 count test에는 정상 입력과 함께 의도적으로 추가 allocation 한 개, wrapper 한 개, lock 한 번, executor post 한 번을 넣은 음성 대조를 둔다. 정상 평균만으로 회귀를 숨기지 않고 메시지 kind별·즉시/pending별 count를 검사한다. 현재 `test_cpp_framework_send_allocations`, mailbox FIFO/owner dispatch, handler-entry permit, operation registry와 request wire identity 검사를 출발점으로 하되 이 이름들이 새 설계 count를 이미 검증한다고 주장하지 않는다. 미래 test가 위 개수나 byte 상한을 넘으면 PR은 실패다. 성능이 통과해도 scope·gate·deadline 계약 실패를 허용하지 않는다.

## 8. 리팩토링 점검

| 중복 검사·handoff·copy·wrapper | 현재 file:line | 설계 판정 |
|---|---|---|
| Local identity를 task로 조회한 뒤 transport 결정 | `C/host/app.cpp:2642`; `C/mesh/mesh_node_runtime.cpp`의 `routing_id_async` | Bind 때 불변 identity를 보관한다. Pending identity coroutine은 초기화 경로에 유지하며 정상 message 절감으로 frame 삭제를 계산하지 않는다. |
| 같은 peer의 Object Client 분류와 live admission 조회 | `C/mesh/mesh_node_runtime.cpp:4197`; `C/mesh/raw_mesh_node_owner.cpp:1765` | 두 의미를 유지한다. 불변 role 자료와 현재 수락을 한 결과로 cache하지 않는다. |
| Source의 parts·payload 중간 표현 | `C/channels/channel_runtime.cpp:1482`; `C/host/app.cpp:2613`; `C/protocol/service_wire_codec.cpp:3005` | 소유자와 binding 경계를 직접 연결한다. Native parts를 사용하며 body 전체 복사였다고 주장하지 않는다. |
| Binding admission→Framework TCS→shared task→logical send state | `C/backend/raw_route_port.cpp:242`; `C/mesh/raw_mesh_node_owner.cpp:1697,1730` | 변환 전용 비동기 결과 제거. 기존 observer coroutine과 logical entry 하나는 유지한다. |
| Native RID→byte vector→native RID | `C/backend/raw_route_port.cpp:495,524`; `C/mesh/mesh_record_dispatcher.cpp:70` | Public received의 typed RID와 reply capability를 보관해 왕복 변환 제거다. |
| Receive마다 lifecycle mutex·port 참조 | `C/mesh/raw_mesh_node_owner.cpp:3609` | Receive 회전의 lease 한 번으로 이동한다. Socket/poller close gate를 삭제하는 근거로 쓰지 않는다. |
| Service header 재분류 | `C/mesh/raw_mesh_node_owner.cpp:3647`; `C/stateful/public_host_runtime.cpp:8165` | Canonical record의 검증된 kind·owner를 재사용한다. 보안/shape 검증의 최초 실행은 유지한다. |
| Tracing 때 service·application·JSON header 재해석과 재포장 | `C/mesh/raw_mesh_node_owner.cpp:3298,3305,3317` | 검증 결과·flow context를 전달한다. 정상 Off 비용 절감으로 tracing-on 중복 전체를 계산하지 않는다. |
| Off에서도 trace 인자 문자열 구성 | `C/mesh/raw_mesh_node_owner.cpp:3649,4242` | 최초 enabled 분기 뒤로 옮긴다. Error 관측을 삭제하지 않는다. |
| Record별 owner 문자열과 ready 목록 복사 | `C/mesh/service_mailbox.cpp:96,109,510` | 등록된 owner entry를 참조한다. Ready membership은 scheduling 소유자 하나가 유지하며 통지용 사본을 없앤다. |
| Prefix claim·records/record_bytes/settled 저장소 | `C/mesh/service_mailbox.cpp:186,190,211` | Node/Channel에서 제거한다. Spot/Actor의 relocation-visible prefix는 유지한다. |
| Owner-wide active/phase로 Node의 다음 take 차단 | `C/mesh/service_mailbox.cpp:158,181,453` | Node/Channel에서 제거하고 record별 소유권만 이전한다. Gate 없는 handler를 직렬 실행해 절감하지 않는다. |
| Claim→host batch→record→route received 이전 | `C/stateful/public_host_runtime.cpp:8123,8557`; `C/mesh/mesh_record_dispatcher.cpp:48,68` | Node/Channel은 canonical record 하나로 통합한다. 이동 횟수와 byte copy를 구별한다. |
| Record마다 mesh/route dispatcher와 reply lambda 구성 | `C/mesh/mesh_record_dispatcher.cpp:53,61` | Registration의 invoker와 binding reply token으로 대체한다. |
| Node도 Spot internal protocol 분류를 경유 | `C/mesh/mesh_node_host_service.cpp:3813` | 등록된 internal packet 소유자 표를 사용하고 ordinary Node/Channel은 직접 route invoker로 간다. Internal 처리 자체를 생략하지 않는다. |
| Send completion std::function·header capture | `C/channels/route_packet_dispatcher.cpp:291,303` | Canonical record에 있는 완료 데이터와 등록된 동작을 사용한다. 메시지별 type-erased closure heap allocation은 0이다. |
| 새 scope와 generic service hash 조회 | `C/channels/route_handler_invoker.cpp:38`; `C/configuration/services.cpp:264,297` | 새 수명은 유지한다. Constructor·dependency slot은 등록 때 준비하고 빈 저장소만 재사용한다. |
| Handler singleton 재사용 | `H/contracts/configuration/mesh_node.hpp:60,581,649`; `C/configuration/services.cpp:299` | API §10과의 불일치를 §6에 보고했다. 이를 유지한 저비용 결과를 정상 계약의 비용으로 세지 않는다. |
| Coroutine frame 밖의 shared state와 첫 observer owner | `H/contracts/dispatch/task.hpp:767,1178` | Frame 안으로 통합한다. Coroutine·재개·결과 수명은 유지한다. |
| Typed request adapter coroutine | `H/contracts/configuration/mesh_node.hpp:643,684` | 유지하고 별도 0.18–0.46 µs를 센다. 병합은 질문으로만 남긴다. |
| Request 변환별 callback capture 저장소 | `H/contracts/dispatch/request_task.hpp:145,717,730`; `C/channels/channel_runtime.cpp:1838,1895` | Call·최종 receiver·logical pending의 다른 수명은 유지하고 변환별 owner/arena는 없앤다. |
| 정상 typed reply의 physical/public 이중 값 보관 | `H/contracts/dispatch/request_task.hpp:696,698`; `H/contracts/codecs/serializer.hpp:85` | 최종 receiver의 값 한 벌을 두 관찰 단계가 사용한다. Public 선행 실패 뒤 physical 처리는 별도로 끝내되 public 결과를 바꾸지 않는다. |
| Logical entry와 native completion entry | `C/foundation/operation_registry.cpp:382`; `B/src/Runtime/Messaging/request_reply.cpp:112` | 서로 다른 authority라 둘 다 유지한다. 같은 terminal 권한을 Framework 두 곳에 만들지 않는다. |
| Deadline 인덱스와 completed flag 중복 위험 | `C/foundation/operation_registry.cpp:129,488,723` | Existing C1 take가 유일한 terminal 권한이다. Deadline 인덱스 참조·public result ready는 각 수명의 사실이며 두 번째 pending 판정에 쓰지 않는다. |
| Per-completion dispatcher post | `C/foundation/operation_registry.cpp:269,281` | 독립 delivery item은 유지한다. 통지만 합칠 수 있으며 callback 직렬 drain으로 바꾸지 않는다. |

점검은 25항목이다. 그중 19항목은 제거·통합·등록/회전 시점 이동 대상, 5항목은 서로 다른 계약 또는 coroutine 수명 때문에 유지하며, handler 수명 1항목은 PM 판정이 필요하다. 이번 paper 작업에서 고친 runtime 항목은 0이다. 범위 밖 이슈 등록이나 코드 수정으로 넘긴 항목은 없다.

수정 전/후 규칙 수의 제안은 Node/Channel 실행 배타 판정 위치 **3→1**(`service_mailbox.cpp:158,181,453`→FIFO head take 한 곳), service-kind 분류 위치 **2→1**(`raw_mesh_node_owner.cpp:3647`, `public_host_runtime.cpp:8165`→ingress 한 곳), source send 중간 비동기 결과 소유 위치 **3→1**(`raw_route_port.cpp:242,243`, `raw_mesh_node_owner.cpp:1697`→최종 logical receiver), 합계 **8→3**이다. 이는 위 세 결정만 센 것이며 branch·호출·mutex 개수나 전체 runtime 규칙 수가 아니다. Physical retry authority, logical C1 take, permit 반환 경계는 각각 **1→1**로 유지한다. 새 frame/slab 저장소 대신 별도 coroutine shared-state block·claim vector·settlement 배열·per-dispatch closure owner를 제거하며, 새 semantic queue·timer·완료 flag는 추가하지 않는다.

재계산 근거는 `analysis/recompute.py`, `analysis/recomputed.json`, `analysis/evidence.json`, `analysis/paper_model.py`, `analysis/paper-sums.json`이다. 앞의 계산기는 입력 JSON·표·텍스트만 읽고, 뒤의 계산기는 명시된 가설의 합만 계산한다. 재현 측정이나 product test를 실행하는 파일은 아니다.

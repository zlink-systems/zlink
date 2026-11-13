# #3868 (+#3867) — C++ Framework 송신 경로 진단

## 기준과 main 변경

체크아웃은 `c81c7b7314`(현재 `origin/main` 스냅샷), detached이며 product 파일 변경은 없다. #4302는 포함됐다. receive-role을 각 실행자의 rotation boundary에서 넘기고 그 boundary에서 ready를 게시한다. 이전 `csend*` 보고서의 수신 role transfer/dispatch 수치는 이 코드에 그대로 적용할 수 없다. 사용자 설명과 달리 #4313 commit은 Git object/local topic branches에는 있지만 `origin/main`/HEAD에는 포함되지 않았다. 그 commit의 same-owner 독립 claim 변경과 현재 HEAD 파일을 대조하면 현재 `service_mailbox.cpp:212–214`가 기존 active message가 남은 같은 owner claim을 막는다. 현재 HEAD/branch 소속과 두 파일은 [head.txt](/root/jobs/fx/cx/v21/s033/d3868/raw/head.txt), [main-log.txt](/root/jobs/fx/cx/v21/s033/d3868/raw/main-log.txt), 대조 결과는 [current-code-check.txt](/root/jobs/fx/cx/v21/s033/d3868/raw/current-code-check.txt)에 기록했다.

30초 active, 세 쌍 중위수에서 send-saturation 1 KiB는 Framework **108,856 msg/s**, binding **571,544 msg/s**, F÷B **0.19046**이다. 각 반복의 측정치는 [bench30.log](/root/jobs/fx/cx/v21/s033/d3868/raw/bench30.log)와 [runs30 결과](/root/jobs/fx/cx/v21/s033/d3868/raw/runs30)에 있다. 요청 경로는 5초 active 중위수 기준 request-backpressure **46,435/148,432 msg/s (0.31284)**, request-serial **8,135/13,544 msg/s (0.60059)**였다. 측정 반복은 [bench-current.log](/root/jobs/fx/cx/v21/s033/d3868/raw/bench-current.log), 결과는 [runs5-current](/root/jobs/fx/cx/v21/s033/d3868/raw/runs5-current)에 있다. 요청에서 제시된 PM 30초 request 수치와는 실행 시간 조건이 다르다.

이전 보고서에서 main이 바뀐 부분 외의 결과는 재실행하지 않았다. `csendb`의 Node-record 독립 실행+단일 조립 변경은 당시 0.2839→0.2737, 합산 CPU 22.05→45.12 µs/msg로 악화했고 multi-drain도 실패했다. 이 실험은 반복하지 않았다. [csendb/report.md](/root/jobs/fx/cx/v21/s033/csendb/report.md)

## 1. 메시지당 결정 소유 표

| 결정 | 현재 판정 위치와 읽기/쓰기 | binding/Core 중복 여부 |
|---|---|---|
| public 호출 1회 수락 | `contracts/channels/call.hpp:38–45` `submit_once_t` atomic claim; `channel_runtime.cpp:1448`, `channel_runtime_state` admission `:344–355`에서 runtime lifecycle 확인 | 물리 send admission과는 별도. 동일한 물리 용량을 재판정하지 않음 |
| 직렬화·wire envelope | typed call `contracts/channels/channel.hpp:835–842`; `channel_runtime.cpp:1521–1528`; codec `client_call_codec.cpp:65`, `client_call_codec.hpp:55–82` | Framework 소유. binding은 전달받은 native message parts를 보냄 |
| 논리 대상/transport 선택 | `channel_runtime.cpp:1498–1503` 등록 transport 선택; `mesh_node_runtime.cpp:4160–4169`, `raw_mesh_node_owner.cpp:4361–4374` 논리 route/peer role | 중복 판정 아님. binding/Core는 RID/socket 기준 물리 경로를 결정하며 Framework의 논리 대상 의미는 모름 |
| source owner turn | `raw_mesh_node_owner.cpp:1653–1664,1685–1705` `state_lane.run_sync` 및 send completion 등록; `state_lane.cpp:151+` turn 소유 | Framework 책임. binding은 이 lane/owner를 알지 못함 |
| socket access + 물리 admission/retry | `raw_route_port.cpp:190–215` Framework `_socket_mutex` 후 binding `async()`; `send_operations.cpp:30–85`, `operation_submit.hpp:55–88`은 native DONTWAIT 시도, EAGAIN/backpressure 분기·completion waiter 결정 | **물리 admission/retry는 binding 소유.** 정상 admission 뒤 Framework가 같은 용량 결정을 중복하지 않음. 별도의 Framework lifecycle admission은 다름 |
| 송신 completion | `raw_mesh_node_owner.cpp:1692–1705` pending completion만 lifecycle에 등록; binding `send_operations.cpp:30–85`가 native operation/completion 소유 | Framework는 route lifecycle 전달, binding은 native 완료를 판정. 완료 사실을 Framework가 다시 추론하지 않음 |
| native receive 및 Framework queue/owner | binding/Core가 native record를 수신; `raw_route_port.cpp:459–496` Framework가 poll/receive 처리와 socket lock `:476`; `service_mailbox.cpp:133–169` enqueue/ready, `:209–277` owner claim | native receive는 binding/Core, logical mailbox/owner는 Framework. 소유 결정이 다름. 단, claim은 현재 `active_messages` guard로 같은 owner의 후속 claim을 제한 |
| wake/dispatch | `service_mailbox.cpp:539–589` ready/notify; `mesh_node_host_service.cpp:2157–2176+` #4302 rotation boundary에서 역할 이동과 ready 공개; dispatcher `route_packet_dispatcher.cpp:203+`, handler `route_handler_invoker.cpp:10+` | socket readiness/native completion wake는 binding/Core, 앱 owner dispatch/ready는 Framework. 별도 Framework native wake 판단은 확인되지 않음 |
| 정산/queue 재활성화 | `service_mailbox.cpp:426–479` terminal atomic settle/credit, mutex 아래 claim 마감; `:539–589` 후속 ready 처리 | Framework mailbox 결정. binding은 native send/receive operation만 종료하며 logical credit를 중복 정산하지 않음 |

중복으로 확인한 부분은 전송 직전 Framework socket mutex/admission 호출 계층과 binding native admission 사이의 **lock/진입 계층 겹침**이다. 물리 admission 정책 자체는 binding 한 곳이 소유한다. Framework `admit_outbound`는 lifecycle만 판단하므로 중복 admission으로 세지 않았다. source owner 경로의 `run_sync`와 Framework socket mutex가 수치상 비싸지만, 여기서는 binding 결정의 중복이라고 판정할 근거가 없다.

## 2. 힙 할당

BPF의 malloc/calloc/realloc 호출 계수로 센 값이다. 추적 중 처리량이 비계측 대비 떨어져 호출 수는 계측된 workload당 추정치이며, frees는 포함하지 않는다. Framework source **16.11회/msg**, target **22.60회/msg**; binding source **7.05**, target **2.42회/msg**. 입력 메시지 수, allocator 호출 합과 분모는 각각 [Framework count capture](/root/jobs/fx/cx/v21/s033/d3868/raw/counts-light/zlink-framework-cpp-send-saturation-1024/run1/zlink-framework-cpp-send-saturation-1024/capture/bpf.txt), [Framework 결과](/root/jobs/fx/cx/v21/s033/d3868/raw/counts-light/zlink-framework-cpp-send-saturation-1024/run1/zlink-framework-cpp-send-saturation-1024/results.json), [binding capture](/root/jobs/fx/cx/v21/s033/d3868/raw/counts-light/zlink-cpp-send-saturation-1024/run1/zlink-cpp-send-saturation-1024/capture/bpf.txt), [binding 결과](/root/jobs/fx/cx/v21/s033/d3868/raw/counts-light/zlink-cpp-send-saturation-1024/run1/zlink-cpp-send-saturation-1024/results.json)에 있다.

| 위치(가장 깊은 심볼화 stack) | 관찰된 할당량 |
|---|---:|
| target Framework `route_handler_invoker_t::invoke` 경로 (`route_handler_invoker.cpp:10+`, route dispatcher 호출) | 한 stack bucket **339,677,184 bytes**; 같은 경로의 별도 bucket **315,414,528 bytes**. site capture 전체 [bpf.txt](/root/jobs/fx/cx/v21/s033/d3868/raw/sites80k/zlink-framework-cpp-send-saturation-1024/run1/zlink-framework-cpp-send-saturation-1024/capture/bpf.txt) |
| Framework source `send_submit_operation_t::async` → `raw_route_port_t::submit_send_impl` → `raw_mesh_node_owner_t::start_send_on_lane` | stack에서 반복 확인. byte bucket은 allocator/stack lookup 오류 때문에 신뢰 가능한 단일 합계로 분리하지 못함. [site capture](/root/jobs/fx/cx/v21/s033/d3868/raw/sites80k/zlink-framework-cpp-send-saturation-1024/run1/zlink-framework-cpp-send-saturation-1024/capture/bpf.txt) |
| native `asio_engine_t::start_async_write` (Framework source), `start_async_read` (binding target) | 해당 함수에서 malloc probe가 관찰됨. binding target read는 **305,918회** (계측 창); per-site bytes는 symbolized 합계로 산출 불가. [site capture](/root/jobs/fx/cx/v21/s033/d3868/raw/sites80k/zlink-cpp-send-saturation-1024/run1/zlink-cpp-send-saturation-1024/capture/bpf.txt) |

Stack profiler는 `failed to look up stack id`도 출력했고, 그 수집은 Framework **39.3k/s**, binding **76.4k/s**까지 처리량을 낮췄다. 따라서 bytes와 callsite는 프로파일링 창에서 확인된 귀속 사례이지, 무계측 비용으로 환산한 값이 아니다. count-only 계측도 Framework **56.5k/s**, binding **307.2k/s**로 낮아졌다. 원본은 [sites.log](/root/jobs/fx/cx/v21/s033/d3868/raw/sites.log), [counts-light.log](/root/jobs/fx/cx/v21/s033/d3868/raw/counts-light.log)에 보존했다.

## 3. 락·futex·atomic

| 범위 | 관찰 횟수/msg | 판정과 주요 위치 |
|---|---:|---|
| Framework source pthread mutex lock | **19.17** | count-only capture; 대부분 uncontended/짧은 획득과 경합을 분리 측정하지 못함. 사용자 정의 `raw_route_port.cpp:192` socket gate와 `state_lane` 동기화 호출 포함 |
| Framework target pthread mutex lock | **13.43** | `service_mailbox.cpp:143` enqueue, `:377` take는 메시지당; `:209` claim과 `:448` settle은 claim/turn당. capture는 위 Framework count 파일 |
| binding source / target pthread mutex lock | **4.69 / 1.63** | binding capture의 mutex probe 합계; Core 내부 callsite별 attribution은 신뢰 가능한 symbol map으로 확정하지 못함 |
| Framework source / target futex syscall | **3.23 / 0.073** | 모든 syscall id 202 합계. 모두 mutex 경합이라는 뜻은 아님. source `state_lane.run_sync` 대기 포함 가능성이 있으며 call graph capture가 이를 보여줌 |
| binding source / target futex syscall | **0.0648 / 0.0034** | 모든 futex syscall 합계; mutex 전용이 아님 |
| 명시된 atomic RMW | 송신 public call당 claim `exchange` **1회**; target settle당 `exchange` + `fetch_sub` 각 **1회** | `contracts/channels/call.hpp:42`, `service_mailbox.cpp:429,434`. BPF 집계에는 일반 atomic 연산 probe가 없으므로 그 외 Core/Framework atomic 총수는 미측정 |

락/futex 수는 count-only capture의 정수 count와 결과 메시지 수를 나눈 계측 workload 수치다 ([Framework](/root/jobs/fx/cx/v21/s033/d3868/raw/counts-light/zlink-framework-cpp-send-saturation-1024/run1/zlink-framework-cpp-send-saturation-1024/capture/bpf.txt), [binding](/root/jobs/fx/cx/v21/s033/d3868/raw/counts-light/zlink-cpp-send-saturation-1024/run1/zlink-cpp-send-saturation-1024/capture/bpf.txt)). 프로브가 처리율을 크게 낮춰 uncontended/contended 비율과 uninstrumented 횟수로 일반화할 수 없다. 별도의 80k paced CPU 계측 조건 및 pinning은 [cpu80k.log](/root/jobs/fx/cx/v21/s033/d3868/raw/cpu80k.log)에 있다.

## 4. #3867 요청 경로와 공유되는 일

| 공유/추가 구간 | request-backpressure | request-serial |
|---|---|---|
| 공유: typed serialization/envelope, transport lookup, source owner turn, route port/socket gate, binding native admission/completion, target receive/mailbox/handler dispatch | 공유한다. backpressure는 특히 binding의 DONTWAIT/EAGAIN 및 native completion 분기를 자주 밟음 | 공유하되 보통 요청/응답을 순서대로 완료 |
| 요청 전용: correlation/deadline, outstanding request 등록, 응답 route/codec, reply completion | 등록된 pending request와 그 만료·완료 정산이 추가됨 | 요청 후 응답 decode/route/reply를 매회 기다려 반복 |
| 현재 계측한 성능(5초 active) | F/B **0.31284** | F/B **0.60059** |

그러므로 send-saturation에서 측정한 단방향 route/target 비용은 두 request 경로에 공통으로 걸릴 수 있지만, backpressure 결과에는 binding completion과 retry가, serial 결과에는 Framework 요청/응답 등록 및 reply round-trip이 더해진다. 수치는 [bench-current.log](/root/jobs/fx/cx/v21/s033/d3868/raw/bench-current.log)와 [runs5-current](/root/jobs/fx/cx/v21/s033/d3868/raw/runs5-current)에 있다.

## 5. 메시지당 CPU 예산과 초과

30초 send-rate의 역수와 별도로, 아래 CPU는 80k paced capture에서 source 0–7, target 8–15에 pin하고 프로세스 user+kernel CPU tick을 완료 메시지 수로 나눈 값이다. 각 프로세스/구현 3회 중위수. 캡처 원본은 [cpu80k.log](/root/jobs/fx/cx/v21/s033/d3868/raw/cpu80k.log) 및 [cpu80k 결과·before/after](/root/jobs/fx/cx/v21/s033/d3868/raw/cpu80k)에 있다.

| CPU, µs/msg | Binding 기준 | Framework 현재 | 차이 |
|---|---:|---:|---:|
| source user | 5.582 | 12.525 | +6.943 |
| source kernel | 10.136 | 15.594 | +5.458 |
| target user | 3.772 | 18.449 | +14.677 |
| target kernel | 7.206 | 9.581 | +2.375 |
| 양쪽 합계 | **26.881** | **56.023** | **+29.142** |
| 목표 예산 = binding ÷ 0.6 | **44.802** | — | Framework 초과 **11.221** |

이 CPU 합계는 위 표의 각 행 중위수를 합한 값이다. 별도로 30초 처리량의 reciprocal에서 얻는 Framework 메시지 시간은 **9.186 µs**, 0.6×binding 목표는 **2.916 µs**, 차이는 **6.270 µs/msg**다. 이는 throughput 시간 환산이며 CPU와 같은 지표가 아니다. 30초 반복은 [bench30.log](/root/jobs/fx/cx/v21/s033/d3868/raw/bench30.log), CPU 원본은 위 80k capture다.

측정된 Framework 초과를 직접 항목별로 배분할 수 있는 프로파일은 없다. CPU 총량과 계측 결과가 지목하는 우선 후보 범위는 (a) target Framework user path의 +14.677 µs/msg (mailbox/owner/dispatch, handler invocation 포함), (b) source user path의 +6.943 (serialization/owner lane/submit), (c) 양단 kernel +5.458 및 +2.375 (전송/수신 syscall와 scheduling) 순이다. 각 차이는 binding 대응 프로세스 대비이며 독립적인 원인 비용의 합으로 해석하면 안 된다. 이전 csend stage profile은 target app drain 6.047µs/msg, source caller +3.708, source Core I/O +3.535, target ingress +3.215, target Core I/O +2.794의 후보 상한을 제시했으나 #4302 전 수치이므로 현재 원인 배분으로 쓰지 않는다. [csend/report.md](/root/jobs/fx/cx/v21/s033/csend/report.md)

후보 제거 순위(측정된 초과 범위를 기준으로 한 조사 우선순위): target dispatch/handler+mailbox 귀속 비용(상한 +14.677), source serialize/owner/submit(+6.943), 양단 kernel/socket 처리(+7.833 합). 이는 겹치는 상한이며 각 제거의 예상 절감 µs/msg는 현재 자료로 확정할 수 없다. 새로운 설계 제안은 하지 않는다.

---

검증: 요청대로 product 코드 변경과 test 실행은 없었다. 구성/빌드 로그와 exit status는 [configure.log](/root/jobs/fx/cx/v21/s033/d3868/raw/configure.log), [build.log](/root/jobs/fx/cx/v21/s033/d3868/raw/build.log), [build.exit](/root/jobs/fx/cx/v21/s033/d3868/raw/build.exit)에 있다. 이 보고서 작성 후 생성한 build directory는 제거했다.

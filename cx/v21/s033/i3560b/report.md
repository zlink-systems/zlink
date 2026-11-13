# #3560 — C++ Framework의 Core wake 발생 경로

작업 경로 `W=/root/worktree/zlink-v21-s033/i3560b`, 원본 경로 `J=/root/jobs/fx/cx/v21/s033/i3560b`. 아래 상대 source 경로는 W, 측정 경로는 J 기준이다. 코드·스펙 변경은 없다.

## 결과

**Framework의 짧은 completion 처리 회전이 Core의 재알림 경로를 자주 실행한다. 물리 wake의 수정 소유자는 Core이며, 분류는 B(기존 결함)다.** Binding bench는 같은 application thread에서 64건 제출마다 poller 구동으로 돌아간다(`framework/bench/grpc/cpp/client/bench_cpp_client.cpp:821–865,583–597`). Framework는 요청별 admission을 관찰하는 제출 loop와 독립된 ingress poll loop를 사용한다(`:1084–1110`; `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp:2158–2162,2395`). API 계수에서 completion drain당 REQUEST는 **binding 165451/2586=63.98개, Framework 68165/26869=2.54개**였다(`raw-api/zlink-{cpp,framework-cpp}-run-{01,02}/wake-counts.txt`).

- Source는 양쪽 모두 **ROUTER 1개·socket poller registration 1개**다. Binding event mask는 32(COMPLETION), Framework는 227(IN·OUT·COMPLETION·ROUTE·CONNECT)이다. Target은 binding ROUTER 2개(request/미사용 command)·poller 0개, Framework ROUTER 1개·socket registration 1개다. 등록을 request마다 반복하지 않는다(`raw-api/registration.txt`; 생성·등록: `client/bench_cpp_client.cpp:485–494`, `zlink/bench_zlink_cpp_server.cpp:134–145`는 모두 `framework/bench/grpc/cpp/` 아래; `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:129–133`).
- `zlink_request`와 completion receive의 flags는 양쪽 모두 DONTWAIT다. Source 일반 ROUTER recv는 양쪽 모두 0회다. Target 일반 recv만 binding NONE, Framework DONTWAIT다. 따라서 source의 빈 일반 recv나 blocking completion receive가 원인은 아니다(`raw-api/.../wake-counts.txt`; binding drain: `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:689–711`). ROUTER–ROUTER의 별도 Completion connection은 **Core 내부 transport lane**이며 두 구현에 공통이다. Framework가 reply용 socket/poller를 추가한 것이 아니다(`core/doc/spec/core/socket/README.ko.md:1288–1290`; 등록 로그).
- Native poller가 completion을 진행한 뒤 public `zlink_poller_wait`가 `get_events()`로 **다시 진행·readiness를 판정**한다. 이어 binding이 호출한 `zlink_completion_recv(DONTWAIT)`도 `process_submit_commands()`를 실행한다. 두 경로는 `primary_poller_drain_` 기본값 false로 batch를 처리하여, **현재 실행 중인 동일 poller의 primary signaler에 재신호**한다(`core/src/runtime/core/socket_poller.cpp:555`; `core/src/api/monitoring/poller_api.cpp:401–420`; `core/src/runtime/sockets/common/socket_base_api.cpp:830`; `core/src/api/socket/socket_message_handler_api.cpp:127`; `core/src/runtime/sockets/common/socket_base_lifecycle.cpp:622,562`; 기본값 `socket_base.hpp:1318`은 같은 디렉터리). 실제 두 호출 스택은 `callchain-evidence.txt:48–75`, 전체는 `raw-wakeperf/.../perf-stacks.txt`에 있다.

상세 BPF 창의 성공 `zlink_request`는 binding **134464**, Framework **49494**건이다. 아래는 실제 `signaler_t::send` 진입 수다. syscall 왕복 수와 다르다.

| 프로세스·호출 경로 | Framework 횟수 / request | binding 횟수 / request |
|---|---:|---:|
| source I/O/2 → activate_read → mailbox | 22518 / 0.4550 | 2097 / 0.0156 |
| source completion 게시 → notify_request_completion → mailbox | 22496 / 0.4545 | 2112 / 0.0157 |
| source public poller의 get_events 재확인 → batch 알림 | 17248 / 0.3485 | 2099 / 0.0156 |
| source completion pull·request submit → batch 알림 | 19327 / 0.3905 | 2110 / 0.0157 |
| **source 합계** | **81589 / 1.6485** | **8418 / 0.0626** |
| target I/O/0 → activate_read → mailbox | 28096 / 0.5677 | 7125 / 0.0530 |
| target reply 제출 thread → batch 알림 | 8493 / 0.1716 | 0 / 0 |

Framework source의 네 번째 행은 completion pull **6954**, request submit **12373**회다. 명령 값 5는 activate_read, 9는 request_completion이다(`core/src/runtime/core/command.hpp:35,39`). 원본: `raw-stacks/zlink-{cpp,framework-cpp}-run-{01,02}/{wake.bt,wake-counts.txt,threads-before.json}`. Source read/write는 Framework **62416/26950**회(1.2611/0.5445 per request), binding **4203/2100**회(0.0313/0.0156)였다. 모든 Framework source 신호의 signaler 주소 `123150939489816`은 mailbox `123150939489712 + 0x68`이다. 즉 이전 보고서의 shared signaler 목록만이 아니라 **primary eventfd 경로**다(`core/src/runtime/core/mailbox.cpp:97,476–478`; `mailbox-send.asm`, `process-commands.asm`). 이 상세 창에서 timer 신호는 0회다.

가벼운 probe에서는 source가 binding **0.0746**, Framework **1.8959 send/request**였다. Binding을 Framework CPU 회차의 처리율 45180 req/s로 제한하자 실제 38890 req/s에서 **1.0456**으로 증가했다. 같은 socket/poller 구성에서도 제출 간격이 병합을 크게 바꾼다. 처리율 제어만으로 Framework와 같아지지는 않았다(`raw-light*/.../wake-counts.txt`, `matched-rate.json`, `evidence-summary.json`).

Source IO/0은 요청 송신, IO/2는 Completion connection 회신 수신이다(`perf-summary.json`). Target I/O CPU는 전체를 signaler 비용으로 설명할 수 없다. BPF 없이 CPU 표본만 수집한 회차의 값은 다음과 같다(µs/완료 request).

| 조건 | 실제 req/s | source IO/0 | source IO/2 | target IO/0 | target IO/1 |
|---|---:|---:|---:|---:|---:|
| binding | 235828 | 2.897 | 3.259 | 2.194 | 3.861 |
| Framework | 45180 | 16.466 | 14.277 | 13.842 | 17.515 |
| binding, 제출률 제한 | 41892 | 6.913 | 4.813 | 6.102 | 5.589 |

- **target IO/0 = request 수신·decode·pipe flush·수신자 알림.** Framework `signaler_t::send`를 포함한 CPU 표본은 **56/1215=4.6%**다. 별도 syscall 창에서 positive recv/request는 binding **0.3454**, Framework **0.8751**이고, signaler write는 거의 0 대 **0.4464**다. Binding target은 직접 blocking receive, Framework target은 FD poller를 구동한다(`raw-syscalls/.../wake-counts.txt`; `perf-summary.json`; `core/src/runtime/engine/asio/asio_engine.cpp:453`, `core/src/runtime/core/mailbox.cpp:70–97`).
- **target IO/1 = Completion connection reply 전송·송신 재개 command 처리.** Framework CPU 표본의 **87.7%**가 `start_async_write` 경로를 포함한다. `process_mailbox` 포함 비율은 Framework **38.0%**, binding **7.6%**다(포함 비율이므로 서로 더하지 않는다). sendto/request는 **0.3283→0.8462**, reply byte/request는 **1252→1406**이다. 즉 byte 증가보다 작은 전송 묶음·재개 횟수 증가가 크다(`perf-summary.json`, `raw-cpu/.../perf-stacks.txt`, `raw-syscalls/.../wake-counts.txt`; `core/src/runtime/engine/asio/asio_engine.cpp:680`, `core/src/runtime/core/io_thread.cpp:54–70`).

이번 target 합계는 **6.055→31.357 µs/request(5.18배)**다. 기존 약 4배와 동일한 절대값을 재현한 회차는 아니다. 45180 req/s를 목표로 한 binding 합계도 **11.691**까지 증가하지만 Framework와 **2.68배**가 남는다. 작은 completion 회전, target의 poller 알림, 작은 TCP 전송 묶음이 관찰된 비용 경로이며, CPU 격차 전부가 잘못된 wake 때문이라는 결론은 성립하지 않는다.

## 판정 위치(전/후 수)

소유 계약은 `core/doc/spec/core/05-polling.ko.md` §3의 level/lost-wake·descriptor 소비자 규칙(**83–91**), §4의 단일 completion owner·drain 규칙(**122–152**), `core/doc/spec/core/systems/11-synchronization-model.ko.md` §3.3의 **이미 깨어 있는 소비자에게 알리지 않는 규칙(187–212)**이다. Framework S08 §4.4는 수신 역할과 continuation 실행 자원 배치만 소유한다(`framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md:300–326`).

| 결정 | 현재 판정 위치 전부(이번 경로) | 수정 시 단일 소유자 | 전→후 목표 |
|---|---|---|---:|
| completion queue가 ready인가 | `core/src/api/socket/socket_completion_queue_internal.cpp:334–346` | 기존 Core completion queue | 1→1 |
| poller의 completion 진행·readiness 확정 | native `socket_base_api.cpp:893–917` + public 재확인 `poller_api.cpp:412–420` → `socket_base_api.cpp:830–851` | Core socket completion-progress owner. Public poller wrapper는 그 결과 변환만 담당 | **2→1** |
| 알림 pending을 언제 해제하는가 | `socket_base_api.cpp:846,912`; `socket_message_handler_api.cpp:156–162` | 같은 Core completion-progress owner의 알림 수명 판정 | **3→1** |
| ready를 알릴 필요가 있는가 | 게시 `socket_request_reply_internal.cpp:455–466` → `socket_base_dispatch.cpp:325–338`; command batch `socket_base_lifecycle.cpp:559–562`. Pull 진입도 `socket_message_handler_api.cpp:127` → `socket_base_lifecycle.cpp:607–622`에서 후자를 실행 | Core completion-progress/mailbox 경계에서 소비자 진행 상태와 ready 전이를 함께 판정 | **2→1** |
| primary poller 선택 | `core/src/runtime/core/socket_poller.cpp:301–305` | 기존 Core poller registration owner | 1→1 |
| primary/등록 목록에 신호를 보낼 조건 | `core/src/runtime/core/mailbox.cpp:83–97,123–125,472–487`의 세 발행 진입점 | 기존 Core mailbox의 공통 wake 판정 | 3→1 |
| 실제 syscall 병합·소비 | `core/src/runtime/core/signaler.cpp:200–224,424–449` | 기존 Core signaler의 물리 edge 병합 | 1→1 |
| Framework의 poll/drain 구동 | `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:129–133,418–465`; binding `poller.cpp:372–404` → `completion_owner.cpp:689–711`(둘 다 `bindings/cpp/src/Runtime/` 아래) | 기존 Framework I0 수신 역할 + binding drain owner. Core 물리 wake 판정은 맡지 않음 | Core wake 판정 **0→0** |

표의 축약 source 파일명은 각각 `core/src/runtime/sockets/common/`, `core/src/api/monitoring/`, `core/src/api/socket/`, `core/src/runtime/core/`의 위에서 적은 파일을 뜻한다. **후 수는 구현 제안의 목표이며 실측 수정 후 값이 아니다. 실제 판정 위치 수는 모두 전과 동일하다.**

**변경 분류 B·수정 계층 Core.** 중복 progress/readiness 판정과 이미 진행 중인 소비자에 대한 wake를 기존 Core owner에 모으는 방향이다. Framework에서 별도 awake flag·poller·대기 묶음을 만들어 보상하는 안은 C(우회)다. 계약은 이미 있으므로 D(spec gap)가 아니다(`supervisor-guide.txt:234`). 구현과 절감량은 미검증이다.

## 검증

- 빌드: `/root/jobs/fx/cx/cpp-build-slot.sh bash J/build.sh`의 실제 절대 경로는 `commands.txt`. Release `-O3 -DNDEBUG`, `-j3`, `ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON`, worktree 내부 build, **125/125·exit 0**. `build.sh`, `build.log`, `CMakeCache.txt`, `binaries.sha256`, `bin/` 보존.
- 실행: `flock -o /root/jobs/fx/cx/measure.lock env PROBE_MODE=stacks python3 J/measure.py`; 추가 DWARF·제출률 대조는 같은 lock 아래 `bash J/finish-runs.sh`. 실제 명령은 `commands.txt`. request-backpressure 1024 B, warmup 5초·active 8초, source CPU 0–7·target 8–15. 모든 **14 cell에서 errors·abandoned·header failures·rejected=0, contamination·drain-bound=false**(`evidence-summary.json`, 각 `raw-*/run1/*/results.json`).
- BPF 횟수 분모는 같은 **2초 gate의 성공 zlink_request 반환**이다. CPU 분모는 before/after 완료 수이며 **TID union**으로 합산했다. Source I/O TID는 binding 4·Framework 8, target은 각각 4다. 모든 I/O TID가 양 endpoint에 존재했다(`summary.json`, 각 `threads-{before,after}.json`, `{source,target}-io-cpu.json`).
- Framework revision **0f2f937e7ffc3f85b76a0d324859aeacf322a42c**. 이전 i3560 이후 completion dispatcher worker 수가 CPU 수 기준에서 2개로 바뀌었다(`framework/languages/cpp/framework/src/runtime/dispatch/provider_task_context.cpp:55`; `identity.txt`). 관련 Core wake 파일은 candidate와 동일하다. 실행 maps·ldd의 Core는 지정 `prefix-1.24`, SHA-256 **2e0aa58e221f0cbe4755d440b20f9c71dee67f692b2fee554d44a913f8323d64**다. Package metadata/SONAME 파일명은 1.23.0이며 source는 **0005a4d6c07160bbf661a6f7aea3d8093880514e**다(`identity.txt`, `ldd-client.txt`, 각 `core-maps-{source,target}.txt`).
- 정확 계수 명령 값 5는 activate_read, 9는 request_completion이다(`core/src/runtime/core/command.hpp:35,39`). 원본: `raw-stacks/`, `raw-light/`, `raw-api/`, `raw-syscalls/`의 `wake.bt`, `wake-counts.txt`; CPU 원본: `raw-cpu/`, `raw-cpu-matched/`의 `perf.data`, `perf-stacks.txt`; DWARF 경로: `raw-wakeperf/`. 집계 코드는 `summarize.py`, `analyze-perf.py`, `evidence-summary.py`다. 빌드 디렉터리는 제거하고 worktree는 남겼다.

## 리팩토링 점검

리팩토링 점검: 성능 2건, POSDDD 1건, 불필요 코드 0건 발견. 고친 항목 0, 넘긴 항목 Core의 중복 readiness·wake 판정과 호출 회전 분산에 따른 전송 비용.

## 남은 문제

- 이전 **1.462** 창에는 호출 스택이 없으므로 그 역사적 횟수의 정확한 소급 분해는 불가능하다. 이번에는 상세 **1.6485**, 가벼운 probe **1.8959**를 각각 원본과 함께 보고했다. Probe 부하·worker 수·회전 밀도가 다르므로 서로 같은 창의 수치로 합치지 않는다.
- DWARF wake trace는 손실이 binding **29**, Framework **7066 sample**이었다(`raw-wakeperf/.../perf-report.txt`). 경로 확인에만 사용했고 위 정확 횟수는 별도 BPF 창에서 얻었다. CPU trace 손실은 0이다. 초기 perf의 숨겨진 심볼명 등록 실패는 ELF file offset **0xb0330**으로 해결했다(`stacks-launch.log`, `run-wakeperf.sh`, `finish-launch.log`).
- Core 수정·수정 후 성능은 미검증이다. 특히 남은 target I/O CPU 격차를 모두 Core wake 수정으로 없앨 수 있다는 근거는 없다. Framework의 수신 독립성을 유지하는 상태에서 Core owner 통합과 실제 전송 묶음 비용을 검증해야 한다.

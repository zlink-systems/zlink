# #4328 — registered-poller disconnect 지연과 assertion 뒤 hang 진단

## 1. 재현 조건과 100/200 ms 상수

| 조건 | 실행·disconnect 표본 | 0 ms | 약 100 ms | 약 200 ms | 나머지 |
|---|---:|---:|---:|---:|---:|
| 유휴 | 100회 × 회당 20개 = 2,000개 | 2,000 | 0 | 0 | 0 |
| `stress-ng --cpu 16` 부하 | 100회 × 회당 20개 = 2,000개 | 2,000 | 0 | 0 | 0 |

표본별 값과 집계 정의는 [distribution-summary.txt](/root/jobs/fx/cx/v21/s033/d4328/logs/distribution-summary.txt), 실행별 결과는 [idle 로그](/root/jobs/fx/cx/v21/s033/d4328/logs/idle-001.log) 및 [load 로그](/root/jobs/fx/cx/v21/s033/d4328/logs/load-001.log)에 보존했다. 두 조건 모두 100회 실행이 rc=0이었다. 부하에서는 각 실행 중 `stress-ng --cpu 16`을 돌렸다. 기존 전체 CTest 기록에는 같은 케이스가 p50=0, p95=207, 최대 208 ms로 실패한 뒤 60초 timeout이 났다([ctest-full.log](/root/jobs/fx/cx/v21/s033/p4244/logs/ctest-full.log), 실행별 재현은 미발생).

`core/src`에서 disconnect 경로의 200 ms timer/poll 기본값은 찾지 못했다. 관련 상수는 `asio_poller_t::loop()`의 무 ready-event 대기 상한 100 ms이며, `options.cpp`의 기본 reconnect 간격도 100 ms다([Core 상수 검색](/root/jobs/fx/cx/v21/s033/d4328/logs/core-100-200-timeouts.txt), [Core의 200 검색 결과](/root/jobs/fx/cx/v21/s033/d4328/logs/core-200-values.txt)). 테스트는 reconnect 간격을 50 ms로 지정하고 disconnect p95 허용값을 200 ms로 둔다([테스트 소스](/root/worktree/zlink-v21-s033/d4328/core/tests/integration/test_socket_disconnect_progress_without_app_poll.cpp:18)). 따라서 200 ms는 이 경로의 Core timer가 아니라 테스트 한계다. 207 ms는 100 ms fallback wait가 연속으로 두 번 걸리는 상황과 양립하지만, 이번 4,000개 표본에서는 지연이 재현되지 않아 누적 원인은 확정하지 못했다.

부하 중 strace 표본은 disconnect 2–13 ms였고 p95=12 ms였다([representative.log](/root/jobs/fx/cx/v21/s033/d4328/logs/representative.log)). syscall timestamp에서 `epoll_wait(..., 100)`은 fd 이벤트를 받아 빠르게 반환했다([representative.strace](/root/jobs/fx/cx/v21/s033/d4328/logs/representative.strace)). 따라서 이 trace는 100 ms 상한과 fd wake 경로를 확인하지만, 207 ms slow sample의 timestamp 증거는 아니다. 강제 실패 시 저장한 gdb stack에서도 I/O thread가 `epoll_wait(..., timeout=100)`에 진입해 있는 것을 확인했다([forced-failure-002.gdb.txt](/root/jobs/fx/cx/v21/s033/d4328/logs/forced-failure-002.gdb.txt)).

## 2. 결정 소유 표

| 결정 | 소유 위치와 읽는 상태 | wake source / timer 대기 지점 |
|---|---|---|
| TCP close를 disconnect로 감지 | [`asio_engine_t::on_read_complete()`](/root/worktree/zlink-v21-s033/d4328/core/src/runtime/engine/asio/asio_engine.cpp:941)는 비정상 read completion을 `error(connection_error)`로 넘긴다. | peer close는 비동기 TCP read의 fd readiness로 전달된다. I/O poller의 `_io_context.poll()`에서 ready event가 없을 때만 `run_for()`의 최대 100 ms timer 대기로 간다([asio_poller.cpp](/root/worktree/zlink-v21-s033/d4328/core/src/runtime/engine/asio/asio_poller.cpp:449)). |
| disconnect를 한 번만 claim하고 DISCONNECTED emit | [`asio_engine_t::error()`/`emit_disconnected()`](/root/worktree/zlink-v21-s033/d4328/core/src/runtime/engine/asio/asio_engine.cpp:1915)는 `_connection_facade.terminating` guard를 확인하고 session의 socket pipe를 통해 claim한다. [`pipe_t::try_claim_transport_disconnected_event()`](/root/worktree/zlink-v21-s033/d4328/core/src/runtime/core/pipe_transport.cpp:719)는 transport connection ID를 atomic exchange로 읽고 중복 여부를 정한다. | 여기서 event를 내보낼지는 connection 종료 callback이 결정한다. 등록된 app poller의 waiter 유무는 이 claim 상태가 아니다. |
| monitor event를 queue에 넣고 poll waiter 깨우기 | [`socket_base_t::event_disconnected()`](/root/worktree/zlink-v21-s033/d4328/core/src/runtime/sockets/common/socket_base_monitor.cpp:452)가 event record를 만들고 [`enqueue_monitor_event()`](/root/worktree/zlink-v21-s033/d4328/core/src/runtime/sockets/common/socket_base_monitor.cpp:756)로 전달한다. worker task는 control runtime에 delay 0으로 예약된다([socket_monitor_runtime.cpp](/root/worktree/zlink-v21-s033/d4328/core/src/runtime/sockets/common/socket_monitor_runtime.cpp:198)). | monitor socket의 mailbox/readiness가 `zlink_poll()` waiter를 깨운다. 테스트 waiter 자체는 3초 deadline으로 monitor socket을 기다리며, 200 ms timer를 쓰지 않는다([테스트 wait](/root/worktree/zlink-v21-s033/d4328/core/tests/integration/test_socket_disconnect_progress_without_app_poll.cpp:107)). |

## 3. assertion 뒤 hang

원래 CTest 실패는 p95=207 ms가 200 ms 제한을 넘은 직후 `Forced closure of 2 sockets`를 출력하고 멈췄다([ctest-full.log](/root/jobs/fx/cx/v21/s033/p4244/logs/ctest-full.log)). 해당 통계 실패는 이번 실행에서 재현되지 않아, 진단용으로 제한 상수만 임시 변경해 같은 assertion 이후 경로를 강제로 실행했다. 이 진단 프로세스는 제한을 임시로 -1로 바꿔 assertion을 강제한 run이며, 실제 통계 실패 재현이 아니다([강제 실패 로그](/root/jobs/fx/cx/v21/s033/d4328/logs/forced-failure-002.log)). 이 프로세스는 `zlink_ctx_term()`에서 멈췄고, gdb main-thread stack은 `poll(timeout=-1) → signaler_t::wait → mailbox_t::recv → zlink_ctx_term → teardown_test_context`였다([forced-failure-002.gdb.txt](/root/jobs/fx/cx/v21/s033/d4328/logs/forced-failure-002.gdb.txt)). `ctx_term()`은 reaper 완료를 기다리는 동기 호출이다([context_api.cpp](/root/worktree/zlink-v21-s033/d4328/core/src/api/core/context_api.cpp:112), [ctx_termination.cpp](/root/worktree/zlink-v21-s033/d4328/core/src/runtime/core/ctx_termination.cpp:131)).

원인은 우선 테스트의 실패 cleanup 순서로 판정한다. p95 assertion은 poller 제거·파괴와 monitor close보다 앞에 있다([테스트 소스](/root/worktree/zlink-v21-s033/d4328/core/tests/integration/test_socket_disconnect_progress_without_app_poll.cpp:343), cleanup은 같은 파일 347–355행). Unity 실패 처리로 함수 tail이 생략되며, teardown은 추적 중인 server/client 두 socket만 강제 close한다([testutil_unity.cpp](/root/worktree/zlink-v21-s033/d4328/core/tests/testutil_unity.cpp:254)). 강제 close 경고의 두 socket은 테스트가 추적한 server/client이며, 별도 monitor socket과 idle app poller는 명시적 cleanup 대상이다. 반면 app poller의 client 등록은 `zlink_poller_destroy()`에서 분리·해제되고([poller_api.cpp](/root/worktree/zlink-v21-s033/d4328/core/src/api/monitoring/poller_api.cpp:123)), monitor handle도 명시적으로 닫아야 하는데 둘 다 assertion 뒤라 실행되지 않는다. 실제 hang 위치는 event wait가 아니라 context reaper mailbox 대기다. 따라서 Core teardown이 임의로 멈춘 증거보다 실패 시 poller/monitor cleanup을 건너뛴 테스트 원인이 stack과 제어 흐름에 부합한다.

## 4. 수정 제안

**Core 지연:** 현재 증거에서 유일하게 확인된 시간 상한은 [`asio_poller_t::loop()`의 100 ms fallback wait](/root/worktree/zlink-v21-s033/d4328/core/src/runtime/engine/asio/asio_poller.cpp:455)다. 후속 수정은 이 지점에서 async read/fd readiness 또는 필요한 mailbox wake가 poller를 실제로 깨우는지 확인하고, 이미 도착한 종료를 timer window 두 번에 걸쳐 처리하는 경로가 있으면 그 wake/dispatch 결정을 소유한 I/O poller 쪽을 고친다. timeout이나 reconnect interval을 늘리거나 줄이는 처방은 근거가 없다. 이번 실행으로 207 ms 누적 원인을 확정할 수 없으므로 구현 전 slow sample 재현과 event/read completion timestamp 추가 확인이 필요하다.

**테스트 hang:** `run_disconnect_progress_case()`의 p95 assert 전에 poller remove/destroy 및 monitor close를 보장하도록 테스트 cleanup 순서를 바꾼다. 실패 시에도 Unity `tearDown()`이 이 리소스를 닫게 두면 longjmp가 함수 tail을 건너뛰어도 context teardown에 미정리 poller와 monitor가 남지 않는다. 결정 변경 위치는 이 테스트의 assertion 및 cleanup 구간이며 Core teardown 동작 변경은 제안하지 않는다.

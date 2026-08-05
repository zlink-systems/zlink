# Round 66: stream 및 one-way 공통 hot path 재검토

- goal:
  - `MULTI_STREAM/tcp/64`와 one-way 64B 미달 항목을 함께 보되, perf 전용 우회 없이
    core runtime hot path에 남는 단순한 개선 후보만 검증한다.
  - 완료 기준: targeted 64B set에서 하락 항목 없이 순효과가 플러스이거나, 반복 `+5%`
    이상 개선 항목이 인접 set에서 회귀 없이 확인된다. 최종 plan 목표는 별도 full/reduced
    full 검증 전까지 완료 처리하지 않는다.
- 시작 시각: 2026-06-15 KST
- 기준 commit: 현재 작업트리
- 시작 git status:
  - core source diff는 SPOT logical queue 및 part-helper restore 계열만 남아 있다.
  - `framework/languages/dotnet/doc/guide/01-overview.ko.md` 변경과 untracked `_workspace/`,
    다수 perf log는 이번 라운드 범위 밖이다.
- corrected baseline:
  - `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
  - `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- 문제 report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 현재 retained 변경 기준 report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_104531_round65_final_spot_restore_all64_reduced_full.txt`
- 대상 pattern/transport/size:
  - 우선 `STREAM/tcp/64`, `PUBSUB/*/64`, `DEALER_DEALER/*/64`
  - 후보가 있으면 인접 `SPOT/*/64`까지 확인한다.

## 가설

- 가설 1:
  - STREAM tcp/64는 perf server send mutex 제거 진단에서 378k~387k까지 올랐지만, 그 변경은
    perf harness 변경이라 채택할 수 없다.
  - core 쪽에서는 STREAM routed send 또는 session write 경로의 중복 lock/copy가 남아 있을 수 있다.
- 가설 2:
  - PUBSUB와 DEALER_DEALER의 64B one-way 미달은 `pipe_t` enqueue/dequeue 자체보다
    matcher/fanout 앞뒤의 반복 상태 확인 또는 wakeup 정책에서 발생할 수 있다.
- 가설 3:
  - 이전 round에서 단일 pipe fast path, prechecked HWM, empty-subscription pipe 상태 추가,
    mailbox/poller 조정은 실패했거나 하락 항목을 만들었다. 같은 형태의 상태 추가 fast path는
    POSD 기준상 다시 시도하지 않는다.
- 선택한 가설:
  - 먼저 STREAM/core 경로를 read-only로 재추적한다. perf harness에서 보인 mutex 병목과 같은
    현상이 core STREAM routing-id send 경로에도 있는지 확인한다.

## 읽은 코드

- `core/src/api/socket/socket_message_send_api.cpp`
  - `send_stream_message()`는 STREAM 단일 routed send에서 routing id를 `msg_t`에 저장한 뒤
    `s_sendmsg()`를 호출한다.
  - 이 경로는 `stream_api_lock_t`로 `stream_t::_api_mutex`를 먼저 잡는다.
- `core/src/runtime/sockets/stream/stream.cpp`
  - `stream_t::xsend()`는 단일 routed message에서 `_more_out`이 아니고 routing id가 있으면
    `_api_mutex` 없이 route shard lock과 pipe write로 바로 보낸다.
  - 즉 core STREAM fast path는 이미 `_api_mutex`를 피하도록 되어 있지만, C API wrapper가
    같은 mutex를 먼저 잡아 이 이점을 줄이고 있다.
- `core/src/runtime/sockets/common/socket_base_msg.cpp`
  - `socket_base_t::send()`는 non-PAIR에서 `socket_public_send_scope_t`로 lifecycle public API
    sync를 잡고 `xsend()`를 호출한다.
  - 따라서 STREAM 단일 FINAL routed send에서 wrapper의 별도 `_api_mutex`를 없애도 public
    send admission과 close/lifecycle 동기화는 남는다.

## 후보: STREAM 단일 routed send의 중복 API mutex 제거

- 변경 예정 파일:
  - `core/src/api/socket/socket_message_send_api.cpp`
- 변경 이유:
  - 기존 `stream_t::xsend()` fast path가 이미 `_api_mutex`를 피하도록 설계되어 있으므로,
    wrapper에서 같은 mutex를 잡는 것은 중복이다.
  - 새 상태나 새 fast path를 추가하지 않고, 이미 있는 core fast path가 실제로 동작하게 한다.
- perf 전용 변경이 아닌 이유:
  - public C API의 STREAM single FINAL routed send 경로 전체에 적용되는 core API wrapper 개선이다.
  - benchmark 조건이나 perf client/server 코드는 바꾸지 않는다.
- 계약 보존:
  - multipart STREAM send는 여전히 지원하지 않고 `PART_FINAL`만 허용한다.
  - routing id 검증, message guard, send failure 시 message consume 규칙은 유지한다.
  - `socket_base_t::send()`의 lifecycle public API sync와 `stream_t::xsend()` 내부 route shard/pipe
    lock은 유지한다.

## 후보 검증: 기능 회귀로 배제

- build:
  - `cmake --build core/build -j$(nproc)` 통과.
- CTest:
  - command:
    `ctest --test-dir core/build --output-on-failure -R 'test_(stream_socket|stream_threadsafe|stream_send_blocking_wakeup|stream_fastpath|stream_routing_id_size|multi_stream_server_reassembly|multi_socket_contract_regressions|transport_matrix)$'`
  - result:
    8개 중 7개 통과, `test_stream_threadsafe` timeout.
  - timeout 위치:
    - `test_stream_callback_rejects_detach_and_close`: PASS
    - `test_stream_send_is_thread_safe_across_app_threads`: PASS
    - 다음 실행 대상은 `test_stream_send_and_close_race_is_safe`.
- focused rerun:
  - command:
    `ctest --test-dir core/build --output-on-failure -R '^test_stream_threadsafe$' --timeout 120`
  - result:
    동일하게 CTest property timeout 30초에서 `test_stream_threadsafe` timeout.
- 판정:
  - STREAM single routed send에서 wrapper의 `_api_mutex`는 close/send race를 막는 의미가 있다.
  - `stream_t::xsend()` fast path만 보고 wrapper lock을 제거하면 public lifecycle과 STREAM
    close race 계약을 충분히 보존하지 못한다.
  - 성능 측정 전에 기능 회귀가 확인됐으므로 후보를 배제한다.
  - source 변경은 원복했다.

## 원복 확인

- build:
  - `cmake --build core/build -j$(nproc)` 통과.
- CTest:
  - command:
    `ctest --test-dir core/build --output-on-failure -R 'test_(stream_socket|stream_threadsafe|stream_send_blocking_wakeup|stream_fastpath|stream_routing_id_size|multi_stream_server_reassembly|multi_socket_contract_regressions|transport_matrix)$'`
  - result:
    8/8 통과.
- 판정:
  - timeout은 후보 변경 때문이었다.
  - STREAM wrapper `_api_mutex` 제거는 POSD 관점에서도 "복잡성을 줄인 것처럼 보이지만
    숨은 동시성 조건을 호출 경로에 누출하는" 변경이므로 채택하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 최종 source 변경 없음. STREAM mutex 후보는 기능 회귀로 원복했다.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message, mtrie, 포트 파싱, IPC unlink, decoder/message/send guard,
    maxmsgsize 정책을 변경하지 않는다.
  - STREAM 후보는 public send wrapper lock만 임시로 건드렸고, 최종 상태에서는 원복되어
    보안 하드닝 의미 변경이 없다.
- 추가로 실행한 회귀 테스트:
  - 원복 후 STREAM 관련 CTest 8/8 통과.

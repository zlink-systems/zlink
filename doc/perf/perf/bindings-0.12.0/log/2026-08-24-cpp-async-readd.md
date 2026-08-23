# C++ binding `send_complete` async terminal 재추가

## 구현

- `core/include`의 C public header를 C++ vendored include에 동기화하고 byte
  parity를 확인했다. C++ 계약에 `send_submit_operation_t::async()`와
  `routed_send_submit_operation_t::async()`를 다시 노출했다.
- `socket_callback_state_t`가 socket당 하나의
  `zlink_send_complete_handler`와 pending operation anchor를 소유한다. Core
  completion callback은 anchor의 suspension만 완료하며 send/request를 다시
  호출하지 않는다. Core가 inline admission을 완료하면 awaitable은 이미
  terminal 상태가 되어 awaiter가 suspend하지 않는다.
- `timeout(...)`은 `zlink_send_async_options_t::timeout_ms`로 전달하고,
  `async_result_t`의 cancel/drop은 `zlink_send_async_cancel`로 전달했다.
  Core의 timeout/terminal은 C++ `submit_error_t`의 `not_admitted`와 원인 errno로
  표면화했다.
- PUB/XPUB는 `publish_operation_t`/`publish_submit_operation_t`로 분리해
  synchronous `submit()`만 노출했다.
- request는 같은 Core reply bridge를 blocking condvar terminal,
  `submit(request_callback_t)` terminal, 기존 coroutine terminal이 공유하도록
  확장했다. blocking wait는 호출자 thread에서만 수행한다.
- Core `request_result_t`의 protocol/rejected/conflict/busy/invalid-state 결과도
  Core가 정의한 errno로 보존했다. async send의 non-admitted terminal은 C++의
  기존 `submit_result_t::not_admitted`와 Core `terminal_errno` 조합으로 전달한다.

## 테스트와 증거

- `cmake --build bindings/cpp/build --target zlink_cpp -j2` 통과.
- `cmake --build bindings/cpp/build-contract -j2` 통과하고, 전체 실행 파일을
  local `core/build` runtime에 다시 링크했다.
- `cmake --build bindings/cpp/build-sanitizers -j2` 통과
  (`-fsanitize=address,undefined -fno-omit-frame-pointer`).
- C++ contract request/reply에 inline send/routed-send, gated small-HWM
  pending drain, per-op timeout, cancel/drop, close-terminal, blocking request
  reply/timeout, callback exactly-once 검사를 추가했다. small-HWM 검사는
  `ZLINK_CPP_CONTRACT_SMALL_HWM=1`에서 활성화한다.
- Release `ctest --test-dir bindings/cpp/build-contract --output-on-failure`는
  21개 중 10개 통과했다. 7개 sample smoke와 behavior/monitor는 Core bind의
  `Unknown error 505 (errno=1)`, request/reply는 Boost.Asio socket open
  `Operation not permitted`로 중단됐다. socket contract에는 기존 HWM
  accounting assertion(`released.current_accounted_bytes() == 0`)이 남아 있다.
  `ZLINK_CPP_CONTRACT_SMALL_HWM=1` request 실행도 새로 추가한 검사 뒤 같은
  기존 Asio 제한에 도달했다.
- sanitizer CTest는 LSan의 ptrace 제한을 피하기 위해
  `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`로 실행했으며 14개 중 10개가
  통과했다. 나머지 4개는 bind/poller 권한 제한이다. ASan/UBSan 보고는 없었다.
- `bindings/cpp/perf/run_binding_single.sh --pattern DEALER_DEALER
  --transports tcp --msg-sizes 64 --duration 3 --runs 1 --reuse-build`는
  local Core로 링크됐지만 sandbox TCP 제한으로 result line 0,
  `non_zero_exit_1`이었다. 따라서 최근 약 1.8M msg/s 범위와의 비교 및
  별도 async-loop perf 수치는 확보하지 못했다. 결과 파일은
  `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260824_010339_cpp-async-readd-20260824.txt`다.
- `bindings/cpp/src`에는 `std::thread`가 없다. sanitizer와 perf 전체 gate는
  이 실행 환경의 socket restriction 때문에 완전한 runtime evidence는
  확보하지 못했다.

## 미해결 계약 질문

- Core completion 실패를 C++에서 `submit_result_t::not_admitted`로 매핑하는
  별도 normative C++ result 문구는 문서에서 확인하지 못했다. 현재 구현은
  `terminal_errno`/`ETIMEDOUT`을 보존하는 이 매핑을 사용한다.

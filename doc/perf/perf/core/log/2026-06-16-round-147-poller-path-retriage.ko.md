# Round 147: poller path 재검토

- goal: STREAM/tcp 64B와 one-way 공통 경로에서 poller 계층에 남은 POSD-safe 성능 후보가 있는지 다시 확인한다.
- 기준:
  - May 26 full: `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
  - current retained source diff: SPOT SENDSEND 단일 FINAL part fast path만 유지한다.
- 판단 기준:
  - +5% 이상이면 명확한 개선으로 본다.
  - 1~2%라도 하락 항목이 없고 설계 복잡도를 늘리지 않으면 채택 후보로 볼 수 있다.
  - 공개 계약, readiness 의미, 보안 hardening을 바꾸는 후보는 배제한다.

## 확인한 코드

- `core/src/runtime/core/socket_poller.cpp`
  - `wait()`는 OS `poll()`/`select()` 전에 `check_socket_events()`로 zlink socket 내부 이벤트를 먼저 확인한다.
  - 이 pre-check는 일반 fd readiness가 아니라 zlink socket 내부 이벤트를 public poller 결과로 드러내는 의미를 담당한다.
- `core/src/runtime/engine/asio/asio_poller.cpp`
  - ASIO loop는 `execute_timers()` 뒤에 `_io_context.poll()`로 ready handler를 먼저 비차단 처리하고, 처리할 이벤트가 없을 때만 `run_for()`로 대기한다.
  - read/write wait는 이벤트 처리 뒤 enable 상태가 유지될 때만 다시 등록한다.
- `core/src/runtime/core/poller_base.cpp`
  - timer 실행은 `_timers.empty()` fast path가 있고, due timer만 실행한 뒤 다음 timeout을 반환한다.
- `core/src/runtime/services/spot/pubsub/spot_subject_poller.cpp`
  - SPOT poller fd/signal 경로는 queue와 signal arming 상태를 함께 관리한다.

## 과거 후보 재확인

- `doc/plan/perf/core/log/2026-06-15-round-65-oneway-current-baseline.ko.md`
  - `socket_poller_t::wait`의 pre-check 제거 A/B는 아래 테스트를 깨뜨렸다.
    - `test_spot_poller_wait_returns_promptly_after_reply`
    - `test_spot_poller_wait_returns_for_each_reply_in_sustained_request_loop`
    - `test_timer_poller_and_recv`
  - 결론은 pre-check가 poller/timer/spot readiness 기능에 필요하다는 것이었다.
- 같은 라운드의 direct publish drain 후 poller interest refresh 후보도 인접 원복 기준 개선이 아니어서 배제됐다.
- round64의 mailbox/wakeup/poller triage도 mailbox wakeup 병합과 command drain은 이미 현재 구조에 반영되어 있다고 결론냈다.

## POSD 판단

- pre-check 제거는 인터페이스를 단순하게 만드는 변경이 아니라, `socket_poller_t::wait` 호출자가 기대하는 readiness 의미를 깨뜨린다.
- ASIO poller loop에 별도 special case를 추가하는 방식은 transport별 예외를 poller 계층에 끌어올려 정보 은닉을 약하게 만든다.
- SPOT signal arming을 더 공격적으로 건드리는 후보는 pub/sub queue 상태와 fd readiness 지식을 여러 곳에 퍼뜨릴 위험이 있다.

## 결론

- round147에서는 source 변경을 하지 않는다.
- poller 계층에는 현재 기준으로 하락 없이 채택할 수 있는 작은 후보가 보이지 않는다.
- 다음 재검토는 STREAM/tcp 64B 목표와 직접 연결되는 send/recv hot path를 대상으로 한다.

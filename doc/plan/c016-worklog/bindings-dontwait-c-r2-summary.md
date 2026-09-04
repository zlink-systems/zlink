# C 바인딩 DONTWAIT backpressure 계약 정합 결과

## 결과

- `bindings/c/**`의 ordinary SEND 호출부에서 0.16.0 방식의 nonzero SEND completion 대기를 제거했다.
- C public surface는 별도 await/promise/future API가 없는 raw API다. 0.17.0 public header에는 이미 `BACKPRESSURED/EAGAIN + nonzero wait token`, `WRITABLE = 3`, ordinary SEND 성공 시 ID 0·completion 없음, `PENDING_MAX_*`의 REQUEST 전용 범위가 반영되어 있어 header를 수정하지 않았다.
- Perf event loop helper는 application이 정확한 payload와 routed RID를 보관하고, `POLLOUT`/`POLLCOMPLETION` 뒤 completion queue를 `NO_DATA`까지 비운 다음 같은 token·context·RID의 `WRITABLE`에서만 같은 bytes를 다시 제출한다. 재제출이 다시 막히면 새 token으로 같은 절차를 반복한다.
- ROUTER/STREAM의 다른 RID 때문에 발생한 socket-wide `POLLOUT`은 completion queue가 `NO_DATA`이면 구독을 억제하고, 정확한 target의 `POLLCOMPLETION` wake를 기다리게 했다. Live token 동안 처리하지 않을 `POLLIN`도 DEALER server poll set에서 제거했다.
- REQUEST/reply completion 경로와 값·ABI는 변경하지 않았다.

## API·동작 전후 표

| 영역 | 이전 C binding 호출부 | 변경 후 |
|---|---|---|
| Ordinary DONTWAIT SEND 성공 | nonzero ID가 올 수 있다고 보고 SEND completion을 accounting | `ZLINK_SUBMIT_OK`, ID 0만 성공으로 인정하고 completion을 기다리지 않음 |
| HWM·target 준비 전 | Core-owned pending SEND 또는 단순 POLLOUT unblock으로 취급 | `ZLINK_SUBMIT_BACKPRESSURED`, `errno == EAGAIN`, nonzero wait token을 상태값으로 처리하고 payload/RID를 application이 보관 |
| 재개 조건 | `ZLINK_COMPLETION_SEND` 또는 socket-wide POLLOUT | queue를 `NO_DATA`까지 pull하고 같은 token·context·RID의 `ZLINK_COMPLETION_WRITABLE`을 확인한 뒤 동일 payload 재제출 |
| 재제출 backpressure | 기존 pending accounting 지속 | 새 nonzero token을 기억하고 event loop에서 다시 대기 |
| 일반 SEND completion | SEND kind를 소비 | 발행되지 않음. `ZLINK_COMPLETION_SEND = 1`은 ABI enum으로만 유지 |
| ROUTER/STREAM no-route | 실패 종류와 ID를 엄격히 고정하지 않음 | public test에서 `ZLINK_SUBMIT_NOT_CONNECTED`, `EHOSTUNREACH`, ID 0, part 소비, completion 없음 고정 |
| Pending limit | SEND pending 문맥과 혼동 가능 | `ZLINK_OPT_PENDING_MAX_MSGS/BYTES`는 REQUEST pending record 전용으로 문서화 |
| REQUEST/reply | REQUEST completion 처리 | 변경 없음 |
| Completion kind surface | WRITABLE 값 검사 없음 | public surface test에서 `ZLINK_COMPLETION_WRITABLE == 3` 고정 |

## 변경 파일

- Public 계약과 등록
  - `bindings/c/CMakeLists.txt`
  - `bindings/c/tests/test_c_contract_surface.c`
  - `bindings/c/tests/test_c_failure_boundary_contract.c`
  - `bindings/c/tests/test_c_dontwait_backpressure_contract.c`
- Perf helper와 호출부
  - `bindings/c/perf/multi/common/perf_multi_client_helpers.hpp`
  - `bindings/c/perf/multi/common/perf_multi_relay_server.hpp`
  - `bindings/c/perf/multi/common/perf_multi_stream_session.hpp`
  - `bindings/c/perf/multi/src/perf_multi_dealer_dealer_client.cpp`
  - `bindings/c/perf/multi/src/perf_multi_dealer_dealer_server.cpp`
  - `bindings/c/perf/multi/tests/test_perf_multi_metrics.cpp`
- Binding 문서
  - `bindings/c/perf/README.md`

`bindings/c/include/**`에는 tracked 변경이 없으며 `core/include/**`의 8개 파일과 byte-identical 상태다.

## 테스트와 gate

| 검사 | 결과 |
|---|---|
| `ulimit -v 16777216; ZLINK_CORE_SOURCE=local ZLINK_BUILD_JOBS=3 ./bindings/c/tests/run_tests.sh` | contract 9/9, sample-smoke 6/6 green |
| 신규 HWM→BACKPRESSURED/token→peer drain→POLLOUT/WRITABLE→동일 payload 재전송 시나리오 | sleep 없이 최종 실행 파일 5/5 green |
| 변경 C 테스트 3개 `cc -std=c11 -Wall -Wextra -Werror -fsyntax-only` | green |
| 영향 perf target 9개, 기존 `core/build` 링크, `-j2` | build green |
| `perf_multi_metrics_test --repeat until-fail:5` | 5/5 green |
| `git diff --check` | green |
| `diff -qr core/include bindings/c/include` | green, 8/8 mirror |
| tracked 변경 범위 | `bindings/c/**`만 변경 |
| stale runtime `ZLINK_COMPLETION_SEND`·신규 sleep/thread/timer | 없음 |
| 독립 최종 재감사 | actionable correctness issue 없음 |

Core 아래에서는 configure/build/clean을 실행하지 않았고 `scripts/local-package/**`, `--core-version`, git commit/push/checkout도 실행하지 않았다.

## 범위 밖 관찰

표준 gate 실패는 없다. 다만 제공된 `core/build/lib/libzlink.so.0.17.0`에 명시적으로 `ZLINK_ROUTER_OPT_MANDATORY=0`을 설정한 뒤 없는 RID로 DONTWAIT send를 시도하면 확정 spec의 `NOT_CONNECTED/EHOSTUNREACH/ID 0`과 달리 silent-drop `ZLINK_SUBMIT_OK`가 반환됐다. Raw C binding은 Core ABI를 그대로 노출하고 header mirror를 변경할 수 없으므로 이 Core 동작은 본 변경 범위에서 수정하지 않았다. 기본 ROUTER와 유효한 4-byte RID의 STREAM no-route 계약은 public C test에서 green이다.

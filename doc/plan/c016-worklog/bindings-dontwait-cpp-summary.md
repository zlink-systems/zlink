# C++ DONTWAIT backpressure 계약 정합 결과

## 결과

C++ 바인딩의 DONTWAIT send를 D-B79 확정 B 계약에 맞췄다. Core가 packet을 받아들이지 못하면 바인딩은 `BACKPRESSURED/EAGAIN`과 nonzero wait token을 보존한다. public poller가 `POLLOUT`으로 깨어난 뒤 completion queue를 `NO_DATA`까지 비우며 같은 token·context·RID의 `ZLINK_COMPLETION_WRITABLE`을 찾고, 바인딩이 보존한 같은 packet을 다시 제출한다. 정상 admission은 completion ID 0에서 끝나며 SEND completion을 기다리지 않는다.

High-level async send는 event loop가 구동한다. 바인딩이 send 대기를 위해 별도 OS thread, sleep 또는 timer를 만들지 않는다. REQUEST/reply completion 경로는 그대로 유지했다.

## API 전후 비교

| 영역 | 이전 동작 | 변경 후 동작 |
|---|---|---|
| raw DONTWAIT send | nonzero completion ID를 accepted pending SEND로 해석하고 SEND completion을 기다리는 계약 | admission 불가 시 `ZLINK_SUBMIT_BACKPRESSURED`, `errno == EAGAIN`, nonzero wait token을 반환한다. Core는 payload를 보존하지 않는다. |
| raw completion | SEND completion이 send 종료를 알림 | 정상 SEND는 ID 0이며 completion이 없다. credit이 돌아오면 같은 token·context·RID의 `ZLINK_COMPLETION_WRITABLE`이 도착한다. |
| `send().message(...).async()` | ID 0에도 synthetic SEND completion을 만들고, nonzero ID는 SEND completion에 연결 | binding entry가 packet과 token을 보존한다. public `poller_t`가 WRITABLE을 읽으면 같은 packet을 다시 제출하며, admission 즉시 result를 완료한다. |
| async poller 등록 | completion 중심 처리 | async result가 terminal이 될 때까지 `pollout | pollcompletion`을 등록한다. poller는 completion queue를 `NO_DATA`까지 읽는다. |
| `send().message(...).submit()` | Core의 blocking send와 `SNDTIMEO` 사용 | 동일하다. binding-owned deadline이나 retry thread를 추가하지 않는다. |
| REQUEST/reply | REQUEST completion을 기다림 | 동일하다. REQUEST용 fallback completion 처리도 유지한다. |
| `ZLINK_OPT_PENDING_MAX_MSGS/BYTES` | SEND pending과 혼동될 수 있는 설명 | 값과 ABI를 유지하며 REQUEST pending admission 전용으로 설명한다. SEND에는 영향을 주지 않는다. |
| completion kind | WRITABLE public 노출 검증 없음 | raw public enum `ZLINK_COMPLETION_WRITABLE == 3`을 compile-time test로 고정한다. |

## 변경 파일

Runtime와 public contract:

- `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp`
- `bindings/cpp/src/Runtime/Messaging/completion_owner.hpp`
- `bindings/cpp/src/Runtime/Messaging/operation_state.hpp`
- `bindings/cpp/src/Runtime/Messaging/operation_submit.hpp`
- `bindings/cpp/src/Runtime/Messaging/send_operations.cpp`
- `bindings/cpp/src/Runtime/Eventing/poller.cpp`
- `bindings/cpp/include/zlink/Contracts/Messaging/operation_contracts.hpp`
- `bindings/cpp/include/zlink/Contracts/Eventing/poll_event.hpp`

Raw header mirror와 문서:

- `bindings/cpp/include/zlink/socket/api.h`
- `bindings/cpp/include/zlink_enum.h`
- `bindings/cpp/README.doxygen.md`

Test와 실행 script:

- `bindings/cpp/tests/contract/test_cpp_contract_common_header_version.cpp`
- `bindings/cpp/tests/contract/test_cpp_contract_exact_target_retry.cpp`
- `bindings/cpp/tests/contract/test_cpp_contract_optimization_guard.cpp`
- `bindings/cpp/tests/contract/test_cpp_contract_request_reply.cpp`
- `bindings/cpp/tests/contract/test_cpp_perf_application_ready_queue.cpp`
- `bindings/cpp/tests/run_tests.sh`
- `bindings/cpp/samples/run_samples.sh`

## Test와 gate

모든 build와 test는 `ulimit -v 16777216`을 적용했다.

| 검증 | 결과 |
|---|---|
| `cmake --build core/build-dev --target clean` 후 `bash scripts/build-core.sh dev` | 통과 |
| `ZLINK_CORE_SOURCE=local`, `ZLINK_CPP_CORE_BUILD_DIR=core/build-dev`, dev `LD_LIBRARY_PATH`로 `bindings/cpp/tests/run_tests.sh` | contract 15/15, sample smoke 7/7 통과 |
| 변경 contract test 5개 반복 | 5회, 총 25/25 통과 |
| 최종 전체 contract label | 15/15 통과 |
| `git diff --check` | 통과 |
| `bash -n bindings/cpp/tests/run_tests.sh bindings/cpp/samples/run_samples.sh` | 통과 |
| `core/include` ↔ `bindings/cpp/include` raw header `cmp` | 8/8 byte-identical |
| Runtime의 `ZLINK_COMPLETION_SEND` 참조 | 없음 |
| Doxygen HTML 생성 | 환경에 `doxygen` executable이 없어 미실행 |

새 public raw test는 HWM까지 채운 뒤 BACKPRESSURED/EAGAIN과 token을 확인하고, peer drain 후 `POLLOUT`, 같은 token·context·RID의 WRITABLE, 같은 packet 재제출 성공과 duplicate 부재까지 검증한다. High-level test는 public `poller_t` 한 번의 event-loop 진행으로 같은 흐름을 검증한다. 새 시나리오에는 sleep이나 timer를 사용하지 않았다.

코드와 test gate에 남은 실패는 없다. commit, push, checkout, `--core-version`, `scripts/local-package/**`는 실행하지 않았다.

# C++ Single ROUTER_ROUTER_REQREP / tcp — local Core 0.13.2

- Core/runtime: local `0.13.2`, baseline revision `c3fd621065ee1d593edc3e5f9413c9e617451f81`,
  `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 3`, sizes `64,256,1024,65536,131072,262144`,
  one client, balanced auto-HWM, automatic HWM, one I/O thread.
- C baseline: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_165412_cpp-router-router-reqrep-tcp-local0132-baseline-c-20260825.txt`
- C++ before: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_165702_cpp-router-router-reqrep-tcp-local0132-baseline-cpp-20260825.txt`
- C++ 후보 A after: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_170021_cpp-router-router-reqrep-tcp-local0132-candidate-a-bridge-owner-20260825.txt`
- C++ 후보 B after: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_170349_cpp-router-router-reqrep-tcp-local0132-candidate-b-async-bridge-20260825.txt`

## Baseline과 후보 결과

| Pass | Throughput ratio (64B→262144B) | Aggregate | Latency median | 판정 |
|---|---|---:|---:|---|
| before | 67.02%, 57.02%, 39.58%, 69.36%, 79.75%, 99.21% | **68.66%** | **1.58x** | 미달 |
| A | 61.12%, 55.46%, 37.38%, 71.21%, 80.81%, 93.70% | 66.61% | 1.64x | 회귀, 폐기 |
| B | 54.96%, 57.75%, 40.83%, 75.12%, 82.40%, 100.87% | 68.66% | 1.74x | 개선 없음, 폐기 |

후보 A는 accepted request의 completion bridge를 callback-owned 단일 객체로 바꿔 `shared_ptr`
control block과 userdata 간접 할당을 없애려 했다. inline completion과 submit failure의 lifetime을
함께 처리했지만 aggregate가 2.05%p 하락해 유지하지 않았다.

후보 B는 `.async()`의 단일 consumer가 이미 `async_operation_state_t`의 terminal 동기화를
사용한다는 점을 이용해, callback/blocking bridge와 분리하여 mutex·optional staging을 없애려 했다.
작은 size의 하락과 큰 size의 개선이 상쇄되어 aggregate가 before와 동일했고 latency median은
악화됐다. POSDDD 책임 분리는 유효했지만 성능 이득이 없고 새 경로의 복잡성만 남으므로 폐기했다.

## POSDDD·contract 검증

- 두 후보 모두 public interface, message/routing-id ownership, exactly-once completion, close·
  failure semantics, cancellation, concurrency, callback context를 바꾸지 않도록 설계했다.
- 후보 A/B 각각에 대해 `test_cpp_contract_message`, `test_cpp_contract_socket`,
  `test_cpp_contract_request_reply`, `test_cpp_contract_behavior` 4개가 모두 통과했다.
- 후보 A/B는 최종 source에서 모두 제거했다. 따라서 baseline public behavior가 최종 동작이다.

request/reply transport aggregate 목표 85.00%에는 미달하므로 최종 상태는 `미달(68.66%)`이다.
동일 manifest의 C·C++ report와 두 후보 after, contract 결과를 남긴 뒤 다음 항목으로 이동한다.

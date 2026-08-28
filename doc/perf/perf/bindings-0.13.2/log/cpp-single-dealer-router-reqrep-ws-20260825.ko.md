# C++ Single DEALER_ROUTER_REQREP / ws — local Core 0.13.2

- Core/runtime: local `0.13.2`, baseline revision `70b7606b4a143051d03d4f94e6d725c78660fad4`,
  `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 3`, sizes `64,256,1024,65536,131072,262144`,
  one client, balanced auto-HWM, automatic HWM, one I/O thread.
- C baseline: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_173116_cpp-dealer-router-reqrep-ws-local0132-baseline-c-20260825.txt`
- C++ before: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_173254_cpp-dealer-router-reqrep-ws-local0132-baseline-cpp-20260825.txt`
- C++ 후보 B after: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_173741_cpp-dealer-router-reqrep-ws-local0132-candidate-b-async-bridge-20260825.txt`

| Pass | Throughput ratio (64B→262144B) | Aggregate | Latency median | 판정 |
|---|---|---:|---:|---|
| before | 25.08%, 28.82%, 31.73%, 85.39%, 89.18%, 89.86% | **58.34%** | **2.19x** | 미달 |
| A | — | — | — | 폐기: contract 실패 |
| B | 23.90%, 28.13%, 31.95%, 74.39%, 86.55%, 82.58% | 54.58% | 2.38x | 회귀, 폐기 |

후보 A는 DEALER single-part request에서 매 호출의 exact transport-pair 선택을 피하고
`zlink_dealer_request_part`를 stack shallow view로 호출하려 했다. 실패 lvalue ownership은 view가
보호하지만, `test_dealer_request_without_initial_routed_target_is_terminal`이 실패했다. 기존 binding은
initial routed target이 없을 때 `not_connected` terminal을 보장하지만, 후보는 Core의 기본 DEALER
선택을 허용했다. 이는 public terminal contract 변경이므로 성능 측정 전에 제거했다.

후보 B는 `.async()` 완료를 async 전용 bridge로 분리해 managed bridge의 mutex·optional staging을
줄였다. inline completion, cancellation, close, callback/context contract test는 통과했으나 throughput과
latency 모두 회귀해 제거했다.

## 최종 검증

- 후보를 모두 되돌린 final source에서 `test_cpp_contract_message`, `test_cpp_contract_socket`,
  `test_cpp_contract_request_reply`, `test_cpp_contract_behavior`가 4/4 통과했다.
- request/reply aggregate 목표 85.00%와 C++ latency 상한 2.0x에 모두 미달하므로 최종 상태는
  `미달(58.34%)`다. public ownership, exact target terminal, exactly-once, close·timeout·cancel,
  concurrency, callback context를 바꾸는 최적화는 유지하지 않았다.

# C++ Single DEALER_ROUTER_REQREP / tcp — local Core 0.13.2

- Core/runtime: local `0.13.2`, baseline revision `d1184f7d0a17c34d588040b9c9ff25096f41aff2`,
  `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 3`, sizes `64,256,1024,65536,131072,262144`,
  one client, balanced auto-HWM, automatic HWM, one I/O thread.
- C baseline: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_162455_cpp-dealer-router-reqrep-tcp-local0132-baseline-c-20260825.txt`
- C++ before: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_162635_cpp-dealer-router-reqrep-tcp-local0132-baseline-cpp-20260825.txt`
- C++ 후보 A after: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_163735_cpp-dealer-router-reqrep-tcp-local0132-candidate-a-safe-cpp-20260825.txt`
- C++ 후보 B after: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_164045_cpp-dealer-router-reqrep-tcp-local0132-candidate-b-cpp-20260825.txt`

## Baseline과 후보 결과

| Pass | Throughput ratio (64B→262144B) | Aggregate | Latency median | 판정 |
|---|---|---:|---:|---|
| before | 54.81%, 56.33%, 39.33%, 73.69%, 83.82%, 91.44% | 66.57% | 1.70x | 미달 |
| A | 66.10%, 57.12%, 38.78%, 72.29%, 80.36%, 90.54% | **67.53%** | **1.53x** | 미달, 채택 |
| B | 66.04%, 53.73%, 38.33%, 71.61%, 74.34%, 79.16% | 63.87% | 1.70x | 회귀, 폐기 |

후보 A는 public `request().message(part)`의 single-part 경로에서 임시 `vector<message_t>`와
일반 multipart adapter를 피한다. 원본 handle을 Core에 직접 전달하면 실패에서도 Core가 part를
소비해 lvalue ownership 계약을 깰 수 있으므로, stack의 shallow native view만 제출한다. 실패 시
원본은 그대로 유지하고, 성공 시에만 원본 shared reference를 close하여 wrapper를 consumed 상태로
전환한다. multipart와 callback/blocking 경로는 기존 계약을 유지한다.

초기 direct-handle 구현은 read-only review에서 failure ownership 위반이 확인되어 측정 근거에서
제외했고, borrowed view 구현으로 교체했다. 후보 B는 `.async()` completion을 별도 bridge 없이
direct sink로 전달해 allocation·mutex·중간 staging을 줄이려 했으나 63.87%로 회귀해 제거했다.

## POSDDD·contract 검증

- A: single-part와 multipart의 책임 경계를 분리하고, success/failure ownership edge를 코드로
  명시했다. 불필요한 one-part vector/adaptor만 제거했으며 public interface·timeout·close·cancel·
  callback context·정확히 한 번 completion은 변경하지 않았다.
- B: 책임 분리는 명확했지만 성능 회귀가 있어 새 복잡성을 남기지 않고 폐기했다.
- `test_cpp_contract_message`, `test_cpp_contract_socket`, `test_cpp_contract_request_reply`,
  `test_cpp_contract_behavior`가 모두 통과했다.
- request/reply test의 one-shot route 부재 기대값은 local Core 0.13.2의 실제
  `backpressured/EAGAIN` submit verdict와 일치하도록 정정했다. binding은 Core verdict를 변환·
  재시도하지 않는다.

목표 85.00%에는 여전히 못 미치므로 최종 상태는 `미달`이다. 후보 A는 개선·POSDDD 이득이 있어
유지하지만, 이 이득이 미달 판정을 통과로 바꾸지 않는다.

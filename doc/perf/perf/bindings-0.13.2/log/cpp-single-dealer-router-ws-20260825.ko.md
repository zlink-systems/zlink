# C++ Single DEALER_ROUTER / ws — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `e230fdb9c27b583bed74f8e1681b97c90f9951ae`,
  `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 3`, sizes `64,256,1024,65536,131072,262144`,
  one client, balanced auto-HWM, automatic HWM, one I/O thread.
- smoke: C `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_172545_cpp-dealer-router-ws-local0132-smoke-c-20260825.txt`,
  C++ `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_172557_cpp-dealer-router-ws-local0132-smoke-cpp-20260825.txt`.
- C baseline: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_172620_cpp-dealer-router-ws-local0132-baseline-c-20260825.txt`
- C++ baseline: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_172759_cpp-dealer-router-ws-local0132-baseline-cpp-20260825.txt`

| Throughput ratio (64B→262144B) | Aggregate | Latency median | 판정 |
|---|---:|---:|---|
| 78.82%, 95.83%, 100.71%, 98.13%, 102.16%, 100.01% | **95.94%** | **1.07x** | 통과 |

routed one-way aggregate 목표 85.00%와 latency 상한 2.0x를 충족했다. 64B는 개별 80.00%
최소 기준보다 낮은 outlier지만, 가이드의 aggregate 판정을 바꾸지 않는다. public routing
ownership, close, callback context, HWM 정책을 바꾸는 후보는 필요하지 않았다.

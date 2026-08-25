# C++ Single DEALER_DEALER / ws — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `fd9e53057bd93b203d9ed2cc05133daaf128f11e`,
  `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 3`, sizes `64,256,1024,65536,131072,262144`,
  one client, balanced auto-HWM, automatic HWM, one I/O thread.
- smoke: C `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_172051_cpp-dealer-dealer-ws-local0132-smoke-c-20260825.txt`,
  C++ `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_172102_cpp-dealer-dealer-ws-local0132-smoke-cpp-20260825.txt`.
- C baseline: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_172115_cpp-dealer-dealer-ws-local0132-baseline-c-20260825.txt`
- C++ baseline: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_172252_cpp-dealer-dealer-ws-local0132-baseline-cpp-20260825.txt`

| Throughput ratio (64B→262144B) | Aggregate | Latency median | 판정 |
|---|---:|---:|---|
| 82.05%, 96.80%, 96.07%, 94.93%, 98.34%, 100.51% | **94.78%** | **1.03x** | 통과 |

routed one-way aggregate 목표 85.00%, 개별 최소 80.00%, C++ latency 상한 2.0x를 충족했다.
공개 routing ownership, close, callback context, auto-HWM의 변경이나 추가 후보는 필요하지 않았다.

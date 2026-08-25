# C++ Single ROUTER_ROUTER / ws — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `f3660efe6f6d8b5d5b05027a0ebd8122f7ed7d3a`,
  `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 3`, sizes `64,256,1024,65536,131072,262144`,
  one client, balanced auto-HWM, automatic HWM, one I/O thread.
- smoke: C `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_174256_cpp-router-router-ws-local0132-smoke-c-20260825.txt`,
  C++ `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_174308_cpp-router-router-ws-local0132-smoke-cpp-20260825.txt`.
- C baseline: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_174322_cpp-router-router-ws-local0132-baseline-c-20260825.txt`
- C++ baseline: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_174501_cpp-router-router-ws-local0132-baseline-cpp-20260825.txt`

| Throughput ratio (64B→262144B) | Aggregate | Latency median | 판정 |
|---|---:|---:|---|
| 85.88%, 102.31%, 98.10%, 91.79%, 92.17%, 100.30% | **95.09%** | **1.08x** | 통과 |

routed one-way aggregate 목표 85.00%, 개별 최소 80.00%, C++ latency 상한 2.0x를 모두 충족했다.
공개 routing-id ownership, exact target, close, callback context, auto-HWM 정책을 바꾸는 후보는 필요하지 않았다.

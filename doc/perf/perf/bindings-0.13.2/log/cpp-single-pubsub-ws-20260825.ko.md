# C++ Single PUBSUB / ws — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `2d801160aaec1b641d020de94605a86f2b34e236`,
  `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 3`, sizes `64,256,1024,65536,131072,262144`,
  one client, balanced auto-HWM, automatic HWM, one I/O thread.
- smoke: C `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_171455_cpp-pubsub-ws-local0132-smoke-c-20260825.txt`,
  C++ `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_171507_cpp-pubsub-ws-local0132-smoke-cpp-20260825.txt`.
- C baseline: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_171534_cpp-pubsub-ws-local0132-baseline-c-20260825.txt`
- C++ baseline: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_171731_cpp-pubsub-ws-local0132-baseline-cpp-20260825.txt`

| Throughput ratio (64B→262144B) | Aggregate | Latency median | 판정 |
|---|---:|---:|---|
| 94.01%, 101.06%, 102.39%, 94.28%, 99.38%, 97.77% | **98.15%** | **1.06x** | 통과 |

one-way 목표 aggregate 95.00%와 C++ latency 상한 2.0x를 모두 충족했다. public ownership,
exactly-once, close, callback context 또는 HWM 설정을 변경한 후보는 필요하지 않았으며, 이 paired
baseline을 최종 결과로 기록한다.

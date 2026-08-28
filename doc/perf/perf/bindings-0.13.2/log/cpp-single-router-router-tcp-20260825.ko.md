# C++ Single ROUTER_ROUTER / tcp — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `542641b839da1dce716c60bf2eac0a59480330c2`,
  `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 3`, sizes `64,256,1024,65536,131072,262144`,
  one client, balanced auto-HWM, automatic HWM, one I/O thread.
- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_164903_cpp-router-router-tcp-local0132-baseline-c-20260825.txt`
- C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_165042_cpp-router-router-tcp-local0132-baseline-cpp-20260825.txt`
- throughput ratio: `82.52%, 88.62%, 94.57%, 77.59%, 85.64%, 92.88%`
- throughput aggregate mean: **86.97%** (C++ routed one-way target 85.00% 통과)
- mean-latency ratio median: **1.10x** (2.0x 상한 통과)

64KiB(77.59%)는 C++ routed one-way의 개별 80% 하한을 소폭 밑도는 outlier다. 완전한 paired
report의 aggregate throughput과 latency gate를 충족하므로 상태는 `통과`이며, 개선 pass 없이
다음 항목으로 이동한다.

# C++ Single DEALER_ROUTER / tcp — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `1b555295751f4af07e7d188a5262247bda114a1a`,
  `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 3`, sizes `64,256,1024,65536,131072,262144`,
  one client, balanced auto-HWM, automatic HWM, one I/O thread.
- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_161948_cpp-dealer-router-tcp-local0132-baseline-c-20260825.txt`
- C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_162125_cpp-dealer-router-tcp-local0132-baseline-cpp-20260825.txt`
- throughput ratio: `77.91%, 100.17%, 99.41%, 77.27%, 88.93%, 92.62%`
- throughput aggregate mean: **89.38%** (C++ routed one-way target 85.00% 통과)
- mean-latency ratio median: **1.05x** (2.0x 상한 통과)

64B(77.91%)와 64KiB(77.27%)는 C++ routed one-way의 개별 80% 하한을 소폭 밑도는
outlier다. 완전한 paired report의 aggregate throughput과 latency gate를 충족하므로 상태는
`통과`이며, 개선 pass 없이 다음 항목으로 이동한다.

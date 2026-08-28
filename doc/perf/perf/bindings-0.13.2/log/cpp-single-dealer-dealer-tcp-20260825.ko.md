# C++ Single DEALER_DEALER / tcp — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `7321518d22d626afbffbc6d9b7d50616f06992d1`,
  `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 5`, sizes `64,256,1024,65536,131072,262144`,
  one client, balanced auto-HWM, automatic HWM, one I/O thread.
- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_160956_cpp-dealer-dealer-tcp-local0132-final5-c-20260825.txt`
- C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_161237_cpp-dealer-dealer-tcp-local0132-final5-cpp-20260825.txt`
- throughput ratio: `79.63%, 96.84%, 92.81%, 78.94%, 82.43%, 86.87%`
- throughput aggregate mean: **86.25%** (C++ routed one-way target 85.00% 통과)
- mean-latency ratio median: **1.20x** (2.0x 상한 통과)

64B(79.63%)와 64KiB(78.94%)는 C++ routed one-way의 개별 80% 하한을 소폭 밑도는
outlier다. 실행 문서 3절에 따라 완전한 paired report에서 aggregate throughput과 latency
gate를 충족했으므로 상태는 `통과`이며, 개선 pass 없이 다음 항목으로 이동한다.

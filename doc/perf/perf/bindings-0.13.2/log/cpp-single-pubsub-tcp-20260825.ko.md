# C++ Single PUBSUB / tcp — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `438f281aa1c95327720fca0ad92193d121a409eb`,
  `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 5`, sizes `64,256,1024,65536,131072,262144`,
  one client, balanced auto-HWM, automatic HWM, one I/O thread.
- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_152813_cpp-pubsub-tcp-local0132-final5-c-20260825.txt`
- C++ report: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_153122_cpp-pubsub-tcp-local0132-final5-cpp-20260825.txt`
- throughput ratio: `93.93%, 97.32%, 98.48%, 92.53%, 100.05%, 91.25%`
- throughput aggregate mean: **95.60%** (C++ simple one-way target 95.00% 통과)
- mean-latency ratio median: **1.07x** (2.0x 상한 통과)

모든 개별 throughput ratio도 C++ simple one-way의 85% 최소 기준을 충족한다. `status: complete`
paired report이므로 개선 pass 없이 다음 항목으로 이동한다.

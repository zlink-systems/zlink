# C++ Single DEALER_DEALER / wss — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `e1eb9baef2dba10bc3c485ef3992755f02d5035b`,
  `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 5`, sizes `64,256,1024,65536,131072,262144`, one client,
  balanced auto-HWM, automatic HWM, one I/O thread.
- C 64B smoke: `2177.83 Kmsg/s`; C++: `2077.72 Kmsg/s` (95.40%).
- C final: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_182334_cpp-dealer-dealer-wss-local0132-final5-c-20260825.txt`
- C++ final: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_182620_cpp-dealer-dealer-wss-local0132-final5-cpp-20260825.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64B | 91.20% | 1.14x |
| 256B | 97.34% | 0.79x |
| 1024B | 90.86% | 1.11x |
| 65536B | 103.93% | 1.02x |
| 131072B | 96.12% | 1.03x |
| 262144B | 83.83% | 1.20x |

## 최종 판정

- secure C/C++ 5-run median paired 결과에서 throughput aggregate는 **93.89%**, latency median은
  **1.07x**다. routed one-way 85% 및 latency gate를 통과했으므로 public hot path 변경과 후보 A/B는
  필요하지 않다.

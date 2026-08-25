# C++ Single DEALER_ROUTER / wss — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `6cfa7aad3e974d1f741cb5ca55a279571ab930e4`, `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 5`, sizes `64,256,1024,65536,131072,262144`, one client, balanced auto-HWM, automatic HWM, one I/O thread.
- 64B smoke: C `2263.94 Kmsg/s`; C++ `2105.36 Kmsg/s` (93.00%).
- C final: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_183058_cpp-dealer-router-wss-local0132-final5-c-20260825.txt`
- C++ final: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_183422_cpp-dealer-router-wss-local0132-final5-cpp-20260825.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64B | 95.65% | 0.93x |
| 256B | 104.07% | 0.98x |
| 1024B | 98.99% | 1.12x |
| 65536B | 99.39% | 1.01x |
| 131072B | 107.71% | 0.95x |
| 262144B | 91.14% | 1.16x |

## 최종 판정

- secure C/C++ 5-run median paired 결과에서 throughput aggregate는 **99.49%**, latency median은 **0.99x**다. routed one-way 85% 및 latency gate를 통과했으므로 public hot path 변경과 후보 A/B는 필요하지 않다.

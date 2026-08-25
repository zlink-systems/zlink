# C++ Single ROUTER_ROUTER / tls — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `e85d5d63e7bb98b1cc5b5277ec0bcb7a9594f488`, `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 5`, sizes `64,256,1024,65536,131072,262144`, one client, balanced auto-HWM, automatic HWM, one I/O thread.
- 64B smoke: C `2551.26 Kmsg/s`; C++ `2337.10 Kmsg/s` (91.61%).
- C final: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_194410_cpp-router-router-tls-local0132-final5-c-20260825.txt`
- C++ final: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_194702_cpp-router-router-tls-local0132-final5-cpp-20260825.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64B | 86.63% | 1.18x |
| 256B | 106.61% | 1.02x |
| 1024B | 101.16% | 0.93x |
| 65536B | 90.14% | 1.12x |
| 131072B | 93.38% | 1.16x |
| 262144B | 98.72% | 0.95x |

## 최종 판정

- secure C/C++ 5-run median paired 결과에서 throughput aggregate는 **96.11%**, latency median은 **1.07x**다. routed one-way 85% 및 latency gate를 통과했으므로 public hot path 변경과 후보 A/B는 필요하지 않다.

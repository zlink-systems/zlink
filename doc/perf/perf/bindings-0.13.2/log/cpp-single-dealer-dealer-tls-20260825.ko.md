# C++ Single DEALER_DEALER / tls — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `1478293f2397e266560af59ef44595b9353ae7c0`, `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 5`, sizes `64,256,1024,65536,131072,262144`, one client, balanced auto-HWM, automatic HWM, one I/O thread.
- 64B smoke: C `2825.46 Kmsg/s`; C++ `2329.35 Kmsg/s` (82.44%).
- C final: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_192134_cpp-dealer-dealer-tls-local0132-final5-c-20260825.txt`
- C++ final: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_192434_cpp-dealer-dealer-tls-local0132-final5-cpp-20260825.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64B | 84.62% | 0.18x |
| 256B | 98.89% | 1.01x |
| 1024B | 94.83% | 1.10x |
| 65536B | 80.48% | 1.21x |
| 131072B | 86.20% | 1.17x |
| 262144B | 80.34% | 1.25x |

## 최종 판정

- secure C/C++ 5-run median paired 결과에서 throughput aggregate는 **87.56%**, latency median은 **1.14x**다. routed one-way 85% 및 latency gate를 통과했으므로 public hot path 변경과 후보 A/B는 필요하지 않다.

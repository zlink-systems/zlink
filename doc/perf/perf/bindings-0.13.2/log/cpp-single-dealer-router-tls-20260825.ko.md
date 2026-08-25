# C++ Single DEALER_ROUTER / tls — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `5fd4317d72eba11b3491ba54fb1e9cd84f1e9dad`, `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 5`, sizes `64,256,1024,65536,131072,262144`, one client, balanced auto-HWM, automatic HWM, one I/O thread.
- 64B smoke: C `2666.20 Kmsg/s`; C++ `2296.27 Kmsg/s` (86.12%).
- C final: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_192917_cpp-dealer-router-tls-local0132-final5-c-20260825.txt`
- C++ final: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_193201_cpp-dealer-router-tls-local0132-final5-cpp-20260825.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64B | 84.51% | 0.21x |
| 256B | 102.94% | 0.97x |
| 1024B | 97.76% | 1.03x |
| 65536B | 85.58% | 1.21x |
| 131072B | 81.93% | 1.22x |
| 262144B | 87.04% | 1.18x |

## 최종 판정

- secure C/C++ 5-run median paired 결과에서 throughput aggregate는 **89.96%**, latency median은 **1.10x**다. routed one-way 85% 및 latency gate를 통과했으므로 public hot path 변경과 후보 A/B는 필요하지 않다.

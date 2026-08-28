# C++ Single ROUTER_ROUTER / wss — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `0564e537c337248b13e51e2fd5eb240f69122ba2`, `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 5`, sizes `64,256,1024,65536,131072,262144`, one client, balanced auto-HWM, automatic HWM, one I/O thread.
- 64B smoke: C `2453.61 Kmsg/s`; C++ `2248.84 Kmsg/s` (91.65%).
- C final: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_184800_cpp-router-router-wss-local0132-final5-c-20260825.txt`
- C++ final: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_185042_cpp-router-router-wss-local0132-final5-cpp-20260825.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64B | 85.73% | 1.14x |
| 256B | 97.40% | 1.03x |
| 1024B | 95.26% | 1.15x |
| 65536B | 97.82% | 1.02x |
| 131072B | 99.70% | 1.00x |
| 262144B | 92.21% | 1.06x |

## 최종 판정

- secure C/C++ 5-run median paired 결과에서 throughput aggregate는 **94.69%**, latency median은 **1.05x**다. routed one-way 85%와 latency gate를 통과했으므로 public hot path 변경과 후보 A/B는 필요하지 않다.

# C++ Single PUBSUB / wss — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `b7688c325fe4cb765ab595c8f4286c6d431b09c5`,
  `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 5`, sizes `64,256,1024,65536,131072,262144`,
  one client, balanced auto-HWM, automatic HWM, one I/O thread.
- 64B smoke: C `1231.28 Kmsg/s`; C++ `1089.70 Kmsg/s` (88.50%).
- C final: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_181435_cpp-pubsub-wss-local0132-final5-c-20260825.txt`
- C++ final: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_181820_cpp-pubsub-wss-local0132-final5-cpp-20260825.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64B | 95.05% | 1.18x |
| 256B | 96.73% | 1.02x |
| 1024B | 96.39% | 1.06x |
| 65536B | 100.53% | 1.00x |
| 131072B | 101.93% | 0.98x |
| 262144B | 98.90% | 1.12x |

## 최종 판정

- secure transport의 C/C++ 5-run median paired 결과에서 throughput aggregate는 **98.25%**,
  latency median은 **1.04x**다. routed one-way 85% 및 latency gate를 통과했으므로 public hot
  path 변경과 후보 A/B는 필요하지 않다.

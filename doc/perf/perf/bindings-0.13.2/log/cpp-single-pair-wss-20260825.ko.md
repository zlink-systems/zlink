# C++ Single PAIR / wss — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `eff6284a79d43bb687d38251d21c2195447664ac`,
  `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 5`, sizes `64,256,1024,65536,131072,262144`,
  one client, balanced auto-HWM, automatic HWM, one I/O thread.
- 64B smoke: C `2283.26 Kmsg/s`; C++ `2175.41 Kmsg/s` (95.28%).
- C final: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_180556_cpp-pair-wss-local0132-final5-c-20260825.txt`
- C++ final: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_180845_cpp-pair-wss-local0132-final5-cpp-20260825.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64B | 88.18% | 1.14x |
| 256B | 106.16% | 0.99x |
| 1024B | 97.27% | 1.07x |
| 65536B | 96.54% | 1.08x |
| 131072B | 98.92% | 1.04x |
| 262144B | 87.53% | 1.17x |

## 최종 판정

- secure transport는 변동을 줄이기 위해 C와 C++ 모두 5-run median을 사용했다.
- throughput aggregate는 **95.77%**, latency median은 **1.07x**로 PAIR strict 95% 및 latency
  gate를 통과했다. public hot path 변경이나 후보 A/B는 필요하지 않다.

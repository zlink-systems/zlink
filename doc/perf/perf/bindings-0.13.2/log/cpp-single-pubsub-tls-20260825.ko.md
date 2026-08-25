# C++ Single PUBSUB / tls — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `0babb638e98828e4cf801bfbe619e4c0ad5c51d1`, `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 5`, sizes `64,256,1024,65536,131072,262144`, one client, balanced auto-HWM, automatic HWM, one I/O thread.
- 64B smoke: C `1379.40 Kmsg/s`; C++ `1266.57 Kmsg/s` (91.82%).
- C final: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_191206_cpp-pubsub-tls-local0132-final5-c-20260825.txt`
- C++ final: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_191532_cpp-pubsub-tls-local0132-final5-cpp-20260825.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64B | 88.23% | 1.18x |
| 256B | 102.62% | 0.98x |
| 1024B | 94.91% | 1.05x |
| 65536B | 87.35% | 1.16x |
| 131072B | 81.85% | 1.22x |
| 262144B | 97.83% | 1.02x |

## 개선 pass와 최종 판정

- 후보 A: public single-part publish는 이미 `publish_operation_t::message(message_t&)`에서 borrowed source를 stage하고 `zlink_publish_part`를 직접 호출한다. state/vector를 거치는 경로는 두 번째 part부터의 multipart API이며, 이번 single-part benchmark의 hot path가 아니다. 같은 경로를 다시 candidate로 측정하지 않는다.
- 후보 B: outbound attempt mutex나 callback-state lifetime guard 제거, bounded pool cap 확대는 close/concurrency 또는 resource boundary를 바꾸므로 no-go다. 64KiB pool 하향은 PAIR에서 timeout된 global policy라 PUBSUB에만 재도입하지 않는다.
- secure C/C++ 5-run median paired 결과에서 throughput aggregate는 **92.13%**, latency median은 **1.11x**다. latency는 통과하지만 simple one-way 95% throughput 목표에는 미달하므로 상태를 **미달**로 확정한다.

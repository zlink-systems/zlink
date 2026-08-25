# C++ Single PAIR / tls — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `cf231e58f4a5cc793ec94ad54d6e939aaaebb83d`, `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 5`, sizes `64,256,1024,65536,131072,262144`, one client, balanced auto-HWM, automatic HWM, one I/O thread.
- 64B smoke: C `2634.79 Kmsg/s`; C++ `2424.60 Kmsg/s` (92.02%).
- C final: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_190411_cpp-pair-tls-local0132-final5-c-20260825.txt`
- C++ final: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_190653_cpp-pair-tls-local0132-final5-cpp-20260825.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64B | 90.41% | 0.71x |
| 256B | 101.12% | 0.98x |
| 1024B | 99.16% | 1.00x |
| 65536B | 92.09% | 0.92x |
| 131072B | 88.67% | 1.32x |
| 262144B | 81.07% | 1.24x |

## 개선 pass와 최종 판정

- 후보 A: 현재 baseline에는 128KiB–1MiB bounded large-message pool과 process-lifetime singleton 보강이 이미 적용돼 있다. 이는 public message ownership, close/cancel, callback context와 mutex 기반 동시성 경계를 보존한 채 TCP PAIR aggregate를 `90.99%→94.37%`로 개선한 구현이다. 같은 변경을 TLS에서 다시 후보로 측정하지 않는다.
- 후보 B: pool 하한을 64KiB로 낮춘 과거 PAIR 후보는 64KiB에서 timeout되어 25/30 result lines의 partial report를 만들었다. TLS에서만 재도입해도 global pool 정책·resource boundary는 동일하므로 no-go다. outbound attempt mutex/weak lifetime guard 제거 또는 pool cap 확대도 contract/resource boundary를 바꾸므로 제외한다.
- secure C/C++ 5-run median paired 결과에서 throughput aggregate는 **92.09%**, latency median은 **0.99x**다. latency는 통과하지만 PAIR strict 95% throughput 목표에는 미달하므로 상태를 **미달**로 확정한다.

# C++ Single PAIR / tls — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `6455ddaf33385ab85479754c9b30e6bde1a9f8dc`, `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 5`, sizes `64,256,1024,65536,131072,262144`, one client, balanced auto-HWM, automatic HWM, one I/O thread.
- 64B smoke: C `2634.79 Kmsg/s`; C++ `2424.60 Kmsg/s` (92.02%).
- C baseline: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_190411_cpp-pair-tls-local0132-final5-c-20260825.txt`
- C++ baseline: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_190653_cpp-pair-tls-local0132-final5-cpp-20260825.txt`
- C1 pool-bypass C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_222936_cpp-pair-tls-local0132-candidate-pool-off-paired5-c-20260825.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_223234_cpp-pair-tls-local0132-candidate-pool-off-paired5-cpp-20260825.txt`
- C2 try-lock C/C++: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_223618_cpp-pair-tls-local0132-candidate-pool-trylock-paired5-c-20260825.txt`, `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_223902_cpp-pair-tls-local0132-candidate-pool-trylock-paired5-cpp-20260825.txt`
- C clean final: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_224250_cpp-pair-tls-local0132-final-clean-paired5-c-20260825.txt`
- C++ clean final: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_224554_cpp-pair-tls-local0132-final-clean-paired5-cpp-20260825.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64B | 87.07% | 0.73x |
| 256B | 101.23% | 0.95x |
| 1024B | 99.74% | 1.00x |
| 65536B | 94.23% | 1.10x |
| 131072B | 92.22% | 1.22x |
| 262144B | 79.46% | 1.27x |

## 개선 pass와 최종 판정

- baseline: 128KiB–1MiB bounded large-message pool과 process-lifetime singleton은 public message ownership, close/cancel, callback context와 mutex 기반 동시성 경계를 보존한 기존 TCP PAIR 개선이다. raw single lvalue path는 이미 `zlink_send_part(native_handle(part))` direct handoff이므로 binding-side native copy/submit 후보는 없다.
- C1(global pool bypass)은 pool을 전역적으로 끄고 native Core allocation을 쓰는 실제 후보였다. contract 5/5를 통과했지만 paired aggregate가 **91.37%**로 baseline 92.09%보다 회귀해 source를 되돌렸다.
- C2(contention-aware try-lock fallback)는 pool mutex가 Core I/O release callback에 점유된 경우에만 native allocation으로 fallback하고, locked release·exact-size reuse·8MiB in-flight+cached cap·process-lifetime 보호를 보존했다. contract 5/5를 통과했지만 paired aggregate **91.55%**로 회귀해 source를 되돌렸다. raw C++ large-payload 변화만으로는 C control 이동을 제거하지 못하므로 채택 근거가 아니다.
- Sol reviewer는 TLS-specific pool 정책, lock-free/thread-local cache, cap 변경, 64KiB pool 확대(기존 25/30 timeout), liveness guard 제거, benchmark harness 변경을 각각 transport-agnostic responsibility·cross-thread release/ABA·resource boundary·기존 실패·public contract/harness 경계 때문에 no-go로 확정했다.
- clean secure C/C++ 5-run paired 결과는 throughput aggregate **92.33%**, latency median **1.05x**다. latency gate는 통과하지만 PAIR strict throughput 95%에는 미달한다. Sol reviewer가 안전 후보 소진을 최종 승인했으므로 상태는 **보류(미달 92.33%)**다.

# C++ Single DEALER_ROUTER_REQREP / tls — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `e1c06412692c53f56110751942a7b5af143f5ebc`, `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 5`, sizes `64,256,1024,65536,131072,262144`, one client, balanced auto-HWM, automatic HWM, one I/O thread.
- 64B smoke: C `175.64 Kops/s`; C++ `91.55 Kops/s` (52.12%).
- clean C final: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260826_001556_cpp-dealer-router-reqrep-tls-local0132-final-clean-paired5-c-20260826.txt`
- clean C++ final: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260826_001852_cpp-dealer-router-reqrep-tls-local0132-final-clean-paired5-cpp-20260826.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64B | 49.80% | 2.27x |
| 256B | 52.63% | 2.45x |
| 1024B | 45.64% | 2.25x |
| 65536B | 90.57% | 0.88x |
| 131072B | 93.02% | 0.94x |
| 262144B | 94.80% | 1.02x |

## 개선 pass와 최종 판정

- C0 — Router/WSS에서 채택된 self-anchor completion 경로를 현재 TLS source에서 별도 paired 측정했다. aggregate **68.56%**, latency 1.71x로 historical baseline 70.94%보다 회귀했고 단독 채택하지 않았다.
- C1 — Core callback에서 terminal completion과 self-anchor release를 한 mutex 구간으로 결합했다. contract 5/5는 통과했으나 aggregate **69.15%**로 회귀하여 원복했다.
- C2 — freshly received reply part를 no-init `message_t` storage로 직접 adopt하도록 구현했다. 초기 full suite의 HWM timeout timing 실패 뒤 단독 3회 반복은 통과했으나 full pair **68.18%**로 회귀하여 원복했다.
- C3 — scheduler가 없을 때만 inline continuation slot을 사용하고 scheduler 경로는 기존 shared slot을 유지했다. 첫 pair 71.39%였지만 Sol acceptance clean pair가 **68.73%**로 C0 범위에 되돌아 비재현으로 원복했다.
- Sol 최종 review: exact target/fallback, lvalue native-view ownership 복구, close/liveness mutex·weak guard, async result/vector/callback context, benchmark pipeline 변경은 public contract 또는 harness no-go다. C0–C3을 모두 구현·contract·paired/acceptance 검증했으며 안전 후보는 소진됐다.
- 원복 source의 authoritative secure C/C++ 5-run pair는 throughput aggregate **71.08%**, latency median **1.63x**다. latency 2.0x gate는 통과하지만 request/reply throughput 85% 목표에는 미달하므로 Sol 승인에 따라 상태를 **보류(미달 71.08%)**로 확정하고 다음 pattern으로 진행한다.

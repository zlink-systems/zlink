# C++ Single DEALER_ROUTER_REQREP / tls — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `e1c06412692c53f56110751942a7b5af143f5ebc`, `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 5`, sizes `64,256,1024,65536,131072,262144`, one client, balanced auto-HWM, automatic HWM, one I/O thread.
- 64B smoke: C `175.64 Kops/s`; C++ `91.55 Kops/s` (52.12%).
- C final: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_193626_cpp-dealer-router-reqrep-tls-local0132-baseline-c-20260825.txt`
- C++ final: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_193909_cpp-dealer-router-reqrep-tls-local0132-baseline-cpp-20260825.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64B | 50.35% | 2.42x |
| 256B | 51.82% | 2.20x |
| 1024B | 48.97% | 2.12x |
| 65536B | 88.54% | 1.12x |
| 131072B | 95.68% | 1.03x |
| 262144B | 90.31% | 1.06x |

## 개선 pass와 최종 판정

- 후보 A: Core의 일반 DEALER request route 선택으로 exact transport target을 생략하면 initial target 부재·detached target의 terminal/no-reroute와 caller message ownership 계약을 바꾸므로 no-go다.
- 후보 B: 현재 source의 async-only completion bridge(`eff6284a79`)는 async 요청에서 managed bridge의 mutex, condition variable, optional staging 및 callback 상태를 만들지 않는다. 이 구현은 WS paired 개선으로 이미 채택됐고 이번 TLS baseline에도 적용되어 있다. request/reply, exact target, message, socket, behavior contract 테스트 5/5를 다시 통과했다.
- secure C/C++ 5-run median paired 결과에서 throughput aggregate는 **70.94%**, latency median은 **1.62x**다. latency는 통과하지만 throughput 85% 목표에는 미달하므로 상태를 **미달**로 확정한다. B 밖의 lifetime guard 제거, target fallback, pool 확대는 public contract 또는 resource boundary를 변경하므로 채택하지 않는다.

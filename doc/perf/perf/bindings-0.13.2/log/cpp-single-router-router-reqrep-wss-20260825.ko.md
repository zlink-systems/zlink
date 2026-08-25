# C++ Single ROUTER_ROUTER_REQREP / wss — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `6a5d94932ed4c32401ec6fde892adb51d6f5a977`, `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 5`, sizes `64,256,1024,65536,131072,262144`, one client, balanced auto-HWM, automatic HWM, one I/O thread.
- 64B smoke: C `156.05 Kops/s`; C++ `70.23 Kops/s` (45.00%).
- C final: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_185524_cpp-router-router-reqrep-wss-local0132-baseline-c-20260825.txt`
- C++ final: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_185808_cpp-router-router-reqrep-wss-local0132-baseline-cpp-20260825.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64B | 41.85% | 2.69x |
| 256B | 38.80% | 2.51x |
| 1024B | 38.61% | 2.65x |
| 65536B | 89.44% | 1.22x |
| 131072B | 90.03% | 1.10x |
| 262144B | 87.36% | 1.12x |

## 개선 pass와 최종 판정

- 후보 A: raw request가 선택한 initial transport target을 생략하고 Core의 일반 route 선택을 쓰는 경로는 성능 후보에서 제외했다. `test_cpp_contract_exact_request_target`가 보장하는 exact-target, detached target의 terminal/no-reroute 및 실패 시 caller message 보존 계약을 바꾸기 때문이다.
- 후보 B: 현재 source의 async-only completion bridge(`eff6284a79`)는 async 요청에서 managed bridge의 mutex, condition variable, optional staging과 callback 상태를 만들지 않는다. 이 분리는 이전 WS paired 결과에서 throughput `56.86%→59.98%`, latency `2.41x→2.15x`로 검증돼 채택되어 있으며, 이번 WSS baseline도 그 구현으로 측정했다. request/reply, exact target, message, socket, behavior contract 테스트 5/5를 다시 통과했다.
- secure C/C++ 5-run median paired 결과에서 throughput aggregate는 **64.35%**, latency median은 **1.86x**다. latency는 2.0x gate를 통과하지만 throughput 85% 목표에는 미달하므로 상태를 **미달**로 확정한다. B 밖의 mutex/weak lifetime guard 제거, target fallback, pool 확대는 public contract 또는 resource boundary를 변경하므로 채택하지 않는다.

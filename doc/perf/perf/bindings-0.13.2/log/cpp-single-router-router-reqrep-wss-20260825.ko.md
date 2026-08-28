# C++ Single ROUTER_ROUTER_REQREP / wss — local Core 0.13.2

- Core/runtime: local `0.13.2`, revision `d7c7f10c59ba56ec43e12c88e42ea3be402ff7ea`, `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2`
- manifest: C→C++, `--duration 5 --runs 5`, sizes `64,256,1024,65536,131072,262144`, one client, balanced auto-HWM, automatic HWM, one I/O thread.
- 64B smoke: C `156.05 Kops/s`; C++ `70.23 Kops/s` (45.00%).
- C baseline: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_185524_cpp-router-router-reqrep-wss-local0132-baseline-c-20260825.txt`
- C++ baseline: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_185808_cpp-router-router-reqrep-wss-local0132-baseline-cpp-20260825.txt`
- C clean final: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260825_221439_cpp-router-router-reqrep-wss-local0132-final-c2-clean-paired5-c-20260825.txt`
- C++ clean final: `/home/hep7hep7/project/zlink/bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_221741_cpp-router-router-reqrep-wss-local0132-final-c2-clean-paired5-cpp-20260825.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64B | 44.58% | 2.24x |
| 256B | 40.00% | 2.68x |
| 1024B | 40.69% | 2.56x |
| 65536B | 92.76% | 1.07x |
| 131072B | 83.37% | 1.19x |
| 262144B | 88.97% | 1.10x |

## 개선 pass와 최종 판정

- 후보 A(exact target 생략)는 `test_cpp_contract_exact_request_target`가 보장하는 terminal/no-reroute와 실패 시 caller message 보존을 바꾸므로 구현하지 않은 no-go다.
- C1(native reply-part adopt)은 native message 초기화 한 번을 줄이는 실제 구현으로 측정했다. paired final은 66.94%였지만 size별 signal이 control drift와 혼재되어 Sol review에서 폐기했고 source도 되돌렸다.
- C2(self-anchor/co-allocated callback bridge)는 별도 heap bridge 대신 async completion state가 Core raw userdata 수명도 잡게 했다. callback 또는 admission-failure cleanup까지 state를 보존하므로 abandoned awaiter, ownership, exactly-once를 바꾸지 않는다. request/reply, exact target, message, socket, behavior contract 5/5를 통과했고, clean C++ 처리량은 baseline 대비 64B `+6.36%`, 256B `+2.50%`, 1024B `+4.77%`, 65536B `+13.38%`, 131072B `-8.48%`, 262144B `+4.53%`였다. Sol review가 고정비 절감 방향의 실제 개선으로 채택했다.
- C3(scheduler 없는 inline resume slot)은 실제 구현·contract 5/5·paired 측정 후 C2 대비 명확한 throughput 이득이 없고 generic coroutine race 면을 넓혀 폐기했다. C4(router native routing-id cache)는 public `routing_id_t`와 native storage를 중복해 한 번의 reply formatting을 줄이는 것보다 storage/copy/reset 비용이 커질 수 있어 Sol review에서 분석 폐기했다.
- clean secure C/C++ 5-run paired 결과는 throughput aggregate **65.06%**, latency median **1.72x**다. latency gate는 통과하지만 throughput 85%에는 명백히 미달한다. Sol reviewer는 exact target/fallback, weak/mutex liveness guard 제거, ownership·close·callback context·await API 변경, harness 변경 외에는 contract-preserving 후보가 남지 않았음을 최종 승인했다. 따라서 상태는 **보류(미달 65.06%)**다.

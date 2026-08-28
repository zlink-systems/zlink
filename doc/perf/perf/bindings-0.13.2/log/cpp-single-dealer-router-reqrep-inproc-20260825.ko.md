# C++ Single — DEALER_ROUTER_REQREP / inproc (local Core 0.13.2)

- 최종 상태: **미달** — 5-run median throughput aggregate **42.85%**, latency median ratio **1.14x**. request/reply throughput 85% 목표에 미달했다.
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_203241_cpp-dealer-router-reqrep-inproc-local0132-final5-c-20260825.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_203443_cpp-dealer-router-reqrep-inproc-local0132-final5-cpp-20260825.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64 | 40.07% | 0.89x |
| 256 | 41.30% | 0.71x |
| 1,024 | 42.95% | 0.80x |
| 65,536 | 24.98% | 1.97x |
| 131,072 | 45.70% | 1.56x |
| 262,144 | 62.11% | 1.40x |

exact target을 생략하거나 재선택하는 후보는 terminal/no-reroute contract를 깨므로 no-go다. async-only completion bridge와 single-part borrowed native view는 baseline에 반영됐으며, 새 단일 lvalue submit-failure contract test를 포함한 request/reply·target·message·socket·behavior 5개 suite가 통과했다. bridge lock 제거, target cache, pool 확대는 ownership·exactly-once·resource-boundary 위험 때문에 채택하지 않는다.

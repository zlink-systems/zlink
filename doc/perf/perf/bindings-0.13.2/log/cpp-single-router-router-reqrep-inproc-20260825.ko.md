# C++ Single — ROUTER_ROUTER_REQREP / inproc (local Core 0.13.2)

- 최종 상태: **미달** — 5-run median throughput aggregate **44.06%**, latency median ratio **1.10x**. request/reply throughput 85% 목표에 미달했다.
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_204333_cpp-router-router-reqrep-inproc-local0132-final5-c-20260825.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_204632_cpp-router-router-reqrep-inproc-local0132-final5-cpp-20260825.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64 | 42.51% | 0.74x |
| 256 | 46.26% | 0.63x |
| 1,024 | 44.67% | 0.61x |
| 65,536 | 30.61% | 1.46x |
| 131,072 | 46.68% | 1.71x |
| 262,144 | 58.29% | 1.54x |

initial exact target을 생략하거나 재선택하는 후보는 terminal/no-reroute contract를 바꾸므로 no-go다. async-only completion bridge와 safe single-part borrowed native view는 baseline이며, 단일 lvalue submit-failure를 포함한 request/reply·target·message·socket·behavior contract suite 5개가 통과했다. callback/blocking bridge의 mutex 제거, target cache, pool 확대는 ownership·exactly-once·close/concurrency·resource-boundary 위험 때문에 채택하지 않는다.

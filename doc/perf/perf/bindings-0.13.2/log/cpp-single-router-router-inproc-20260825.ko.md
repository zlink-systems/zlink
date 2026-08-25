# C++ Single — ROUTER_ROUTER / inproc (local Core 0.13.2)

- 최종 상태: **미달** — 5-run median throughput aggregate **69.85%**, latency median ratio **1.09x**. strict routed one-way throughput 95% 목표에 미달했다.
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_203821_cpp-router-router-inproc-local0132-final5-c-20260825.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_204014_cpp-router-router-inproc-local0132-final5-cpp-20260825.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64 | 89.22% | 1.27x |
| 256 | 92.60% | 0.27x |
| 1,024 | 87.26% | 0.46x |
| 65,536 | 20.87% | 3.88x |
| 131,072 | 88.95% | 1.06x |
| 262,144 | 86.81% | 1.12x |

128KiB–1MiB bounded pool 및 operation-state 재사용은 이미 baseline이다. 64KiB 하락을 겨냥한 전역 pool 하향은 TCP 25/30 partial timeout, cap 확대는 bounded-resource 책임 경계 위반이라 no-go다. route cache와 lifetime guard/mutex 제거는 exact route와 close/concurrency contract를 약화시키므로 채택하지 않는다.

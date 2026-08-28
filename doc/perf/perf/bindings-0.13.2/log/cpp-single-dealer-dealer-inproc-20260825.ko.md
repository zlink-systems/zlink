# C++ Single — DEALER_DEALER / inproc (local Core 0.13.2)

- 최종 상태: **미달** — 5-run median throughput aggregate **70.49%**, latency median ratio **1.22x**. routed one-way throughput 80% 목표에 미달했다.
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_201932_cpp-dealer-dealer-inproc-local0132-final5-c-20260825.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_202231_cpp-dealer-dealer-inproc-local0132-final5-cpp-20260825.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64 | 76.21% | 0.79x |
| 256 | 84.11% | 1.27x |
| 1,024 | 81.64% | 0.87x |
| 65,536 | 25.90% | 3.00x |
| 131,072 | 72.17% | 1.21x |
| 262,144 | 82.94% | 1.22x |

128KiB–1MiB bounded message pool은 이미 baseline이다. pool 하한을 64KiB로 낮추는 전역 후보는 이전 TCP run에서 25/30 partial report와 timeout을 냈고, 8MiB cap 확대는 bounded-resource 경계를 바꾼다. transport별 resource policy 분기는 적용하지 않아 모두 no-go로 확정한다.

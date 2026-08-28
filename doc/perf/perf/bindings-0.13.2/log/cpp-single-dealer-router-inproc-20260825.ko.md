# C++ Single — DEALER_ROUTER / inproc (local Core 0.13.2)

- 최종 상태: **미달** — 5-run median throughput aggregate **70.97%**, latency median ratio **1.36x**. strict routed one-way throughput 95% 목표에 미달했다.
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_202628_cpp-dealer-router-inproc-local0132-final5-c-20260825.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_202853_cpp-dealer-router-inproc-local0132-final5-cpp-20260825.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64 | 79.52% | 3.38x |
| 256 | 79.73% | 1.22x |
| 1,024 | 92.77% | 1.09x |
| 65,536 | 21.50% | 3.00x |
| 131,072 | 76.30% | 1.43x |
| 262,144 | 80.02% | 1.26x |

128KiB–1MiB bounded message pool은 현 baseline이다. 64KiB를 전역 threshold로 낮추는 후보는 TCP 25/30 partial timeout을 재도입하고, cap 확대는 bounded-resource 책임 경계를 바꾸므로 no-go다. transport별 pool 정책 분기와 mutex/lifetime guard 제거도 동일 lifecycle contract를 바꾸므로 채택하지 않는다.

# C++ Single — PUBSUB / inproc (local Core 0.13.2)

- 최종 상태: **미달** — 5-run median throughput aggregate **78.71%**, latency median ratio **1.23x**. strict PUBSUB throughput 95% 목표에 미달했다.
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_201103_cpp-pubsub-inproc-local0132-final5-c-20260825.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_201420_cpp-pubsub-inproc-local0132-final5-cpp-20260825.txt`

| Size | Throughput ratio | Latency ratio |
|---:|---:|---:|
| 64 | 99.46% | 1.44x |
| 256 | 91.11% | 1.09x |
| 1,024 | 92.14% | 0.88x |
| 65,536 | 24.09% | 3.25x |
| 131,072 | 79.24% | 1.29x |
| 262,144 | 86.24% | 1.17x |

현재 `operation_submit.hpp`의 direct single-part publish와 128KiB–1MiB bounded message pool은 이미 baseline이다. 64KiB를 pool에 편입하는 전역 후보는 이전 TCP run에서 25/30 partial report 후 timeout을 냈고, cap 확대는 bounded-resource 경계를 바꾸므로 no-go다. transport별 정책 분기는 동일 message lifecycle의 자원 정책을 갈라 적용하지 않는다.

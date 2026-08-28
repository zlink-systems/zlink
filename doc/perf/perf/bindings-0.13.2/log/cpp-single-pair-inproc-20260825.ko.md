# C++ Single — PAIR / inproc (local Core 0.13.2)

## 결론

- 최종 상태: **미달**
- 5-run median paired 결과: throughput aggregate mean **76.09%**, latency median ratio **1.08x**
- strict PAIR 기준(throughput 95% 이상, latency 1.5x 이하)에서 latency는 통과했지만 throughput은 미달이다.

## 최종 측정

- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260825_200219_cpp-pair-inproc-local0132-final5-c-20260825.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260825_200530_cpp-pair-inproc-local0132-final5-cpp-20260825.txt`
- Core source: local `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.13.2` (0.13.2)

| Size | C msg/s | C++ msg/s | Throughput ratio | C latency ms | C++ latency ms | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 2,787,138.8 | 2,361,098.8 | 84.71% | 1.306 | 0.277 | 0.21x |
| 256 | 1,782,677.4 | 1,782,799.4 | 100.01% | 1.441 | 1.358 | 0.94x |
| 1,024 | 1,670,147.0 | 1,526,496.4 | 91.40% | 0.408 | 0.241 | 0.59x |
| 65,536 | 359,853.4 | 81,733.0 | 22.71% | 0.008 | 0.026 | 3.25x |
| 131,072 | 154,032.0 | 113,515.2 | 73.70% | 0.014 | 0.018 | 1.29x |
| 262,144 | 67,428.8 | 56,666.8 | 84.04% | 0.023 | 0.028 | 1.22x |

## 개선 pass와 no-go

1. 후보 A — 128KiB–1MiB bounded pool 및 process-lifetime singleton
   - 이미 현재 source의 baseline에 반영됐다. native release callback의 late-call 안전성과 ownership contract를 보존하는 경로라 같은 변경을 새 후보로 반복하지 않는다.
2. 후보 B — global pool 하한을 64KiB로 내리거나 8MiB cap 확대
   - **no-go**: 64KiB 하한은 이전 TCP run에서 25/30 partial report 후 timeout을 낸 전역 후보다.
     inproc에만 다른 threshold를 두면 동일 public message lifecycle의 resource policy가 transport마다 달라진다.
   - cap 확대는 문서화된 bounded-resource 경계를 넓히므로 이번 성능 pass에서는 적용하지 않는다.

64KiB의 큰 차이는 pool 적용 하한과 native/C++ message lifetime 비용의 결합으로 보이지만, 위 두 후보 모두 전 transport의 안정성·resource 계약을 보장하지 못한다. 따라서 변경 없이 미달로 확정하고 다음 패턴으로 이동한다.

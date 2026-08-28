# C++ paired measurement: Single / inproc / DEALER_ROUTER

- timestamp: 2026-08-28T07:21:57+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_072157_cpp0140-single-inproc-dealer-router-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_072227_cpp0140-single-inproc-dealer-router-r1.txt`
- status: 미달(68.0%)
- aggregate throughput ratio: 68.03% (target 85%)
- aggregate mean-latency ratio: 1.621x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 899896.800 | 719086.000 | 79.91% | 4.953 | 7.021 | 1.418x |
| 256 | 768017.400 | 680149.400 | 88.56% | 2.899 | 3.289 | 1.135x |
| 1024 | 804486.600 | 649771.400 | 80.77% | 0.900 | 1.117 | 1.241x |
| 65536 | 346296.200 | 116831.600 | 33.74% | 0.008 | 0.018 | 2.250x |
| 131072 | 179998.400 | 57999.000 | 32.22% | 0.011 | 0.029 | 2.636x |
| 262144 | 74372.200 | 69139.800 | 92.96% | 0.021 | 0.022 | 1.048x |

## 판정 근거

- throughput aggregate 68.03%와 mean-latency aggregate 1.621x를 target 85% / max 2.0x와 비교해 `미달(68.0%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

# C++ paired measurement: Single / ipc / DEALER_DEALER

- timestamp: 2026-08-28T07:29:07+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_072907_cpp0140-single-ipc-dealer-dealer-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_072938_cpp0140-single-ipc-dealer-dealer-r1.txt`
- status: 미달(83.8%)
- aggregate throughput ratio: 83.80% (target 95%)
- aggregate mean-latency ratio: 0.961x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 937911.200 | 752047.400 | 80.18% | 29.856 | 31.204 | 1.045x |
| 256 | 802994.400 | 642837.800 | 80.06% | 0.775 | 0.630 | 0.813x |
| 1024 | 840385.400 | 593053.000 | 70.57% | 2.360 | 1.136 | 0.481x |
| 65536 | 40244.800 | 34208.800 | 85.00% | 0.225 | 0.287 | 1.276x |
| 131072 | 27952.400 | 24868.000 | 88.97% | 0.197 | 0.223 | 1.132x |
| 262144 | 17120.200 | 16785.000 | 98.04% | 0.185 | 0.188 | 1.016x |

## 판정 근거

- throughput aggregate 83.80%와 mean-latency aggregate 0.961x를 target 95% / max 2.0x와 비교해 `미달(83.8%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

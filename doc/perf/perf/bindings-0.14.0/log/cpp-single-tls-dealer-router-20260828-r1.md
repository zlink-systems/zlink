# C++ paired measurement: Single / tls / DEALER_ROUTER

- timestamp: 2026-08-28T07:13:49+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_071349_cpp0140-single-tls-dealer-router-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_071420_cpp0140-single-tls-dealer-router-r1.txt`
- status: 미달(95.5%)
- aggregate throughput ratio: 95.52% (target 85%)
- aggregate mean-latency ratio: 5.421x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 743859.400 | 659478.600 | 88.66% | 136.309 | 168.913 | 1.239x |
| 256 | 854653.200 | 643338.000 | 75.27% | 2.431 | 66.572 | 27.385x |
| 1024 | 372693.400 | 456131.800 | 122.39% | 1.249 | 1.056 | 0.845x |
| 65536 | 12859.200 | 12390.200 | 96.35% | 0.856 | 0.806 | 0.942x |
| 131072 | 7301.800 | 7383.000 | 101.11% | 0.702 | 0.696 | 0.991x |
| 262144 | 4122.200 | 3683.800 | 89.36% | 0.755 | 0.850 | 1.126x |

## 판정 근거

- throughput aggregate 95.52%와 mean-latency aggregate 5.421x를 target 85% / max 2.0x와 비교해 `미달(95.5%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

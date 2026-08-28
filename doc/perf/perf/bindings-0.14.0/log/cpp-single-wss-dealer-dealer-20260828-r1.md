# C++ paired measurement: Single / wss / DEALER_DEALER

- timestamp: 2026-08-28T07:04:04+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_070403_cpp0140-single-wss-dealer-dealer-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_070435_cpp0140-single-wss-dealer-dealer-r1.txt`
- status: 통과(99.3%)
- aggregate throughput ratio: 99.29% (target 95%)
- aggregate mean-latency ratio: 1.113x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 678934.200 | 716068.800 | 105.47% | 165.863 | 132.982 | 0.802x |
| 256 | 758441.600 | 669040.600 | 88.21% | 38.107 | 48.237 | 1.266x |
| 1024 | 262757.400 | 259286.000 | 98.68% | 26.794 | 35.760 | 1.335x |
| 65536 | 9909.600 | 9995.400 | 100.87% | 13.023 | 12.943 | 0.994x |
| 131072 | 6029.400 | 6064.200 | 100.58% | 9.866 | 9.843 | 0.998x |
| 262144 | 3250.800 | 3314.000 | 101.94% | 6.908 | 8.867 | 1.284x |

## 판정 근거

- throughput aggregate 99.29%와 mean-latency aggregate 1.113x를 target 95% / max 2.0x와 비교해 `통과(99.3%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

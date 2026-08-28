# C++ paired measurement: Single / ws / DEALER_DEALER

- timestamp: 2026-08-28T06:54:15+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_065415_cpp0140-single-ws-dealer-dealer-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_065447_cpp0140-single-ws-dealer-dealer-r1.txt`
- status: 미달(91.0%)
- aggregate throughput ratio: 90.98% (target 95%)
- aggregate mean-latency ratio: 1.114x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 854556.800 | 639608.000 | 74.85% | 121.267 | 160.651 | 1.325x | 854556.800/121.267 | 639608.000/160.651 |
| 256 | 698665.000 | 667776.000 | 95.58% | 40.635 | 48.424 | 1.192x | 698665.000/40.635 | 667776.000/48.424 |
| 1024 | 568994.600 | 525829.600 | 92.41% | 16.914 | 16.921 | 1.000x | 568994.600/16.914 | 525829.600/16.921 |
| 65536 | 24971.200 | 22289.800 | 89.26% | 5.036 | 5.683 | 1.128x | 24971.200/5.036 | 22289.800/5.683 |
| 131072 | 15036.400 | 14443.000 | 96.05% | 0.342 | 0.353 | 1.032x | 15036.400/0.342 | 14443.000/0.353 |
| 262144 | 9395.200 | 9183.600 | 97.75% | 0.427 | 0.429 | 1.005x | 9395.200/0.427 | 9183.600/0.429 |

## 판정 근거

- throughput aggregate 90.98%와 mean-latency aggregate 1.114x를 target 95% / max 2.0x와 비교해 `미달(91.0%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

# C++ paired measurement: Single / tls / PAIR

- timestamp: 2026-08-28T07:10:28+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_071027_cpp0140-single-tls-pair-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_071059_cpp0140-single-tls-pair-r1.txt`
- status: 미달(87.5%)
- aggregate throughput ratio: 87.51% (target 95%)
- aggregate mean-latency ratio: 1.073x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 980597.000 | 720230.200 | 73.45% | 92.368 | 142.617 | 1.544x |
| 256 | 922828.600 | 736917.000 | 79.85% | 1.524 | 0.577 | 0.379x |
| 1024 | 373309.600 | 381178.800 | 102.11% | 1.247 | 1.257 | 1.008x |
| 65536 | 12945.000 | 11888.000 | 91.83% | 0.725 | 0.771 | 1.063x |
| 131072 | 7701.800 | 7059.800 | 91.66% | 0.671 | 0.846 | 1.261x |
| 262144 | 4140.400 | 3566.600 | 86.14% | 0.748 | 0.883 | 1.180x |

## 판정 근거

- throughput aggregate 87.51%와 mean-latency aggregate 1.073x를 target 95% / max 2.0x와 비교해 `미달(87.5%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

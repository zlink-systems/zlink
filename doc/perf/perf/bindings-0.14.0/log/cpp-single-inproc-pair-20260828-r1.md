# C++ paired measurement: Single / inproc / PAIR

- timestamp: 2026-08-28T07:18:35+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_071835_cpp0140-single-inproc-pair-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_071906_cpp0140-single-inproc-pair-r1.txt`
- status: 미달(65.9%)
- aggregate throughput ratio: 65.89% (target 95%)
- aggregate mean-latency ratio: 1.819x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 972833.600 | 759315.200 | 78.05% | 4.575 | 7.053 | 1.542x |
| 256 | 889658.000 | 755369.400 | 84.91% | 2.491 | 2.933 | 1.177x |
| 1024 | 865453.000 | 712089.400 | 82.28% | 0.819 | 0.989 | 1.208x |
| 65536 | 366299.200 | 86496.200 | 23.61% | 0.007 | 0.025 | 3.571x |
| 131072 | 173084.400 | 71404.400 | 41.25% | 0.011 | 0.025 | 2.273x |
| 262144 | 73512.000 | 62663.400 | 85.24% | 0.021 | 0.024 | 1.143x |

## 판정 근거

- throughput aggregate 65.89%와 mean-latency aggregate 1.819x를 target 95% / max 2.0x와 비교해 `미달(65.9%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

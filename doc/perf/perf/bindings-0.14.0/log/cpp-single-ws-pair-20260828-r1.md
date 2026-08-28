# C++ paired measurement: Single / ws / PAIR

- timestamp: 2026-08-28T06:51:57+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_065157_cpp0140-single-ws-pair-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_065228_cpp0140-single-ws-pair-r1.txt`
- status: 미달(93.7%)
- aggregate throughput ratio: 93.72% (target 95%)
- aggregate mean-latency ratio: 1.028x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 789856.400 | 726501.400 | 91.98% | 108.695 | 130.236 | 1.198x | 789856.400/108.695 | 726501.400/130.236 |
| 256 | 868837.800 | 711540.800 | 81.90% | 40.885 | 36.668 | 0.897x | 868837.800/40.885 | 711540.800/36.668 |
| 1024 | 549898.200 | 525162.000 | 95.50% | 17.352 | 18.304 | 1.055x | 549898.200/17.352 | 525162.000/18.304 |
| 65536 | 24455.000 | 22794.200 | 93.21% | 5.583 | 5.513 | 0.987x | 24455.000/5.583 | 22794.200/5.513 |
| 131072 | 14784.600 | 14858.400 | 100.50% | 0.345 | 0.358 | 1.038x | 14784.600/0.345 | 14858.400/0.358 |
| 262144 | 9516.000 | 9442.200 | 99.22% | 0.350 | 0.347 | 0.991x | 9516.000/0.350 | 9442.200/0.347 |

## 판정 근거

- throughput aggregate 93.72%와 mean-latency aggregate 1.028x를 target 95% / max 2.0x와 비교해 `미달(93.7%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

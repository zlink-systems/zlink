# C++ paired measurement: Single / ws / PUBSUB

- timestamp: 2026-08-28T06:53:00+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_065259_cpp0140-single-ws-pubsub-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_065338_cpp0140-single-ws-pubsub-r1.txt`
- status: 통과(98.7%)
- aggregate throughput ratio: 98.72% (target 95%)
- aggregate mean-latency ratio: 1.253x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 717775.000 | 605782.600 | 84.40% | 100.376 | 145.114 | 1.446x | 717775.000/100.376 | 605782.600/145.114 |
| 256 | 704825.000 | 592418.200 | 84.05% | 32.386 | 42.587 | 1.315x | 704825.000/32.386 | 592418.200/42.587 |
| 1024 | 473253.000 | 485613.000 | 102.61% | 16.751 | 19.332 | 1.154x | 473253.000/16.751 | 485613.000/19.332 |
| 65536 | 12972.600 | 13015.800 | 100.33% | 0.391 | 0.393 | 1.005x | 12972.600/0.391 | 13015.800/0.393 |
| 131072 | 6060.600 | 7107.200 | 117.27% | 0.276 | 0.515 | 1.866x | 6060.600/0.276 | 7107.200/0.515 |
| 262144 | 2582.800 | 2677.600 | 103.67% | 0.429 | 0.315 | 0.734x | 2582.800/0.429 | 2677.600/0.315 |

## 판정 근거

- throughput aggregate 98.72%와 mean-latency aggregate 1.253x를 target 95% / max 2.0x와 비교해 `통과(98.7%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

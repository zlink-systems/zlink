# C++ paired measurement: Single / ws / DEALER_ROUTER

- timestamp: 2026-08-28T06:55:19+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_065519_cpp0140-single-ws-dealer-router-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_065550_cpp0140-single-ws-dealer-router-r1.txt`
- status: 통과(91.6%)
- aggregate throughput ratio: 91.61% (target 85%)
- aggregate mean-latency ratio: 1.048x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 810831.600 | 617939.200 | 76.21% | 135.926 | 156.248 | 1.150x | 810831.600/135.926 | 617939.200/156.248 |
| 256 | 804450.800 | 664961.000 | 82.66% | 45.085 | 59.703 | 1.324x | 804450.800/45.085 | 664961.000/59.703 |
| 1024 | 546622.600 | 525459.000 | 96.13% | 17.648 | 15.094 | 0.855x | 546622.600/17.648 | 525459.000/15.094 |
| 65536 | 24246.800 | 22184.000 | 91.49% | 5.189 | 5.761 | 1.110x | 24246.800/5.189 | 22184.000/5.761 |
| 131072 | 15063.200 | 14924.600 | 99.08% | 0.348 | 0.349 | 1.003x | 15063.200/0.348 | 14924.600/0.349 |
| 262144 | 9061.200 | 9433.600 | 104.11% | 0.416 | 0.353 | 0.849x | 9061.200/0.416 | 9433.600/0.353 |

## 판정 근거

- throughput aggregate 91.61%와 mean-latency aggregate 1.048x를 target 85% / max 2.0x와 비교해 `통과(91.6%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

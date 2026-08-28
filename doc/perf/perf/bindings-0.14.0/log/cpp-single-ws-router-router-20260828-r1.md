# C++ paired measurement: Single / ws / ROUTER_ROUTER

- timestamp: 2026-08-28T06:57:23+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_065723_cpp0140-single-ws-router-router-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_065755_cpp0140-single-ws-router-router-r1.txt`
- status: 통과(90.3%)
- aggregate throughput ratio: 90.32% (target 85%)
- aggregate mean-latency ratio: 1.204x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 801783.800 | 603474.200 | 75.27% | 133.660 | 178.214 | 1.333x | 801783.800/133.660 | 603474.200/178.214 |
| 256 | 759815.400 | 593753.600 | 78.14% | 34.520 | 53.520 | 1.550x | 759815.400/34.520 | 593753.600/53.520 |
| 1024 | 672198.800 | 616641.600 | 91.74% | 14.499 | 14.535 | 1.002x | 672198.800/14.499 | 616641.600/14.535 |
| 65536 | 24206.200 | 23122.600 | 95.52% | 4.897 | 6.617 | 1.351x | 24206.200/4.897 | 23122.600/6.617 |
| 131072 | 15228.800 | 14964.600 | 98.27% | 4.023 | 4.187 | 1.041x | 15228.800/4.023 | 14964.600/4.187 |
| 262144 | 8722.800 | 8985.600 | 103.01% | 0.370 | 0.350 | 0.946x | 8722.800/0.370 | 8985.600/0.350 |

## 판정 근거

- throughput aggregate 90.32%와 mean-latency aggregate 1.204x를 target 85% / max 2.0x와 비교해 `통과(90.3%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

# C++ paired measurement: Multi / tcp / MULTI_STREAM

- timestamp: 2026-08-28T08:02:15+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_080215_cpp0140-multi-tcp-stream-default-inventory-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_080243_cpp0140-multi-tcp-stream-default-inventory-r1.txt`
- status: 미달(93.5%)
- aggregate throughput ratio: 93.46% (target 95%)
- aggregate mean-latency ratio median: 1.032x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,65536.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 365070.400 | 336609.000 | 92.20% | 0.279 | 0.299 | 1.072x | 365070.400/0.279 | 336609.000/0.299 |
| 256 | 346978.000 | 331822.800 | 95.63% | 0.288 | 0.300 | 1.042x | 346978.000/0.288 | 331822.800/0.300 |
| 1024 | 317650.200 | 310130.600 | 97.63% | 0.316 | 0.323 | 1.022x | 317650.200/0.316 | 310130.600/0.323 |
| 65536 | 59836.600 | 52870.600 | 88.36% | 1.986 | 1.891 | 0.952x | 59836.600/1.986 | 52870.600/1.891 |

## 판정 근거
- throughput aggregate 93.46%와 latency 중앙값 1.032x로 판정했다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

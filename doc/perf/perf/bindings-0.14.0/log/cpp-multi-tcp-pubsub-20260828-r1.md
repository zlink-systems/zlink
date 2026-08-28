# C++ paired measurement: Multi / tcp / MULTI_PUBSUB

- timestamp: 2026-08-28T07:58:19+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_075819_cpp0140-multi-tcp-pubsub-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_075853_cpp0140-multi-tcp-pubsub-r1.txt`
- status: 통과(164.1%)
- aggregate throughput ratio: 164.10% (target 95%)
- aggregate mean-latency ratio median: 0.899x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 511070.000 | 1138849.800 | 222.84% | 1336.743 | 1323.530 | 0.990x | 511070.000/1336.743 | 1138849.800/1323.530 |
| 256 | 605896.600 | 1308410.400 | 215.95% | 1817.088 | 1468.930 | 0.808x | 605896.600/1817.088 | 1308410.400/1468.930 |
| 1024 | 750585.600 | 1433710.600 | 191.01% | 1095.607 | 640.740 | 0.585x | 750585.600/1095.607 | 1433710.600/640.740 |
| 4096 | 617602.600 | 844201.400 | 136.69% | 455.014 | 72.221 | 0.159x | 617602.600/455.014 | 844201.400/72.221 |
| 65536 | 67824.400 | 78602.000 | 115.89% | 139.157 | 153.730 | 1.105x | 67824.400/139.157 | 78602.000/153.730 |
| 131072 | 38183.200 | 39022.200 | 102.20% | 125.186 | 140.618 | 1.123x | 38183.200/125.186 | 39022.200/140.618 |

## 판정 근거
- throughput aggregate 164.10%와 latency 중앙값 0.899x로 판정했다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

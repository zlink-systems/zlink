# C++ paired measurement: Multi / wss / MULTI_ROUTER_ROUTER_SENDSEND

- timestamp: 2026-08-28T08:18:06+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_081806_cpp0140-multi-wss-router-router-sendsend-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_081842_cpp0140-multi-wss-router-router-sendsend-r1.txt`
- status: 통과(123.6%)
- aggregate throughput ratio: 123.59% (target 85%); aggregate mean-latency ratio median: 0.820x (max 2.0x).
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072; duration=5s; part-count=2.
- META in both reports: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded; native SHA matched before measurement.

| Size | C raw (throughput / mean latency ms) | C++ raw (throughput / mean latency ms) | Throughput / latency ratio |
|---:|---:|---:|---:|
| 64 | 86821.600 / 0.646 | 88306.200 / 0.593 | 101.71% / 0.918x |
| 256 | 82867.800 / 0.566 | 108099.400 / 0.432 | 130.45% / 0.763x |
| 1024 | 78337.200 / 0.597 | 103878.000 / 0.451 | 132.60% / 0.755x |
| 4096 | 63786.200 / 0.741 | 84412.000 / 0.666 | 132.34% / 0.899x |
| 65536 | 10612.600 / 4.664 | 13535.400 / 3.660 | 127.54% / 0.785x |
| 131072 | 6581.400 / 7.537 | 7694.800 / 6.452 | 116.92% / 0.856x |

## 판정 근거

- throughput aggregate와 latency 중앙값이 모두 목표를 만족했다.

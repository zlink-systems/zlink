# C++ paired measurement: Multi / tls / MULTI_DEALER_ROUTER_SENDSEND

- timestamp: 2026-08-28T08:28:17+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_082817_cpp0140-multi-tls-dealer-router-sendsend-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_082852_cpp0140-multi-tls-dealer-router-sendsend-r1.txt`
- status: 통과(101.1%)
- aggregate throughput ratio: 101.07% (target 85%); aggregate mean-latency ratio median: 1.089x (max 2.0x).
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072; duration=5s; part-count=2.
- META in both reports: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded; native SHA matched before measurement.

| Size | C raw (throughput / mean latency ms) | C++ raw (throughput / mean latency ms) | Throughput / latency ratio |
|---:|---:|---:|---:|
| 64 | 85731.800 / 0.533 | 93378.600 / 0.535 | 108.92% / 1.004x |
| 256 | 103482.000 / 0.445 | 93101.600 / 0.537 | 89.97% / 1.207x |
| 1024 | 92926.000 / 0.493 | 91637.600 / 0.546 | 98.61% / 1.108x |
| 4096 | 79824.600 / 0.582 | 80188.800 / 0.623 | 100.46% / 1.070x |
| 65536 | 12921.800 / 3.791 | 12941.600 / 4.632 | 100.15% / 1.222x |
| 131072 | 6854.000 / 8.612 | 7422.200 / 6.724 | 108.29% / 0.781x |

## 판정 근거

- throughput aggregate와 latency 중앙값이 모두 목표를 만족했다.

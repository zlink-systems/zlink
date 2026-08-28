# C++ paired measurement: Multi / tls / MULTI_STREAM

- timestamp: 2026-08-28T08:34:37+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_083437_cpp0140-multi-tls-stream-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_083500_cpp0140-multi-tls-stream-r1.txt`
- status: 통과(103.9%)
- aggregate throughput ratio: 103.92% (target 95%); aggregate mean-latency ratio median: 0.951x (max 2.0x).
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,65536; 4096 and 131072 are not registered for this pattern; duration=5s; part-count=2.
- META in both reports: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded; native SHA matched before measurement.

| Size | C raw (throughput / mean latency ms) | C++ raw (throughput / mean latency ms) | Throughput / latency ratio |
|---:|---:|---:|---:|
| 64 | 183991.600 / 0.543 | 194524.400 / 0.514 | 105.72% / 0.947x |
| 256 | 188372.000 / 0.531 | 191487.400 / 0.522 | 101.65% / 0.983x |
| 1024 | 165968.200 / 0.602 | 173694.400 / 0.575 | 104.66% / 0.955x |
| 65536 | 15284.800 / 7.894 | 15841.200 / 6.313 | 103.64% / 0.800x |

## 판정 근거

- throughput aggregate와 latency 중앙값이 모두 목표를 만족했다.

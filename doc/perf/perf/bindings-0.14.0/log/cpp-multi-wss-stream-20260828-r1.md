# C++ paired measurement: Multi / wss / MULTI_STREAM

- timestamp: 2026-08-28T08:22:10+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_082210_cpp0140-multi-wss-stream-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_082232_cpp0140-multi-wss-stream-r1.txt`
- status: 통과(103.9%)
- aggregate throughput ratio: 103.87% (target 95%); aggregate mean-latency ratio median: 0.984x (max 2.0x).
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,65536; 4096 and 131072 are not registered for this pattern; duration=5s; part-count=2.
- META in both reports: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded; native SHA matched before measurement.

| Size | C raw (throughput / mean latency ms) | C++ raw (throughput / mean latency ms) | Throughput / latency ratio |
|---:|---:|---:|---:|
| 64 | 165153.600 / 0.605 | 179811.400 / 0.656 | 108.88% / 1.084x |
| 256 | 171919.400 / 0.581 | 177820.400 / 0.562 | 103.43% / 0.967x |
| 1024 | 162580.600 / 0.615 | 167655.200 / 0.596 | 103.12% / 0.969x |
| 65536 | 9470.600 / 10.563 | 9475.400 / 10.560 | 100.05% / 1.000x |

## 판정 근거

- throughput aggregate와 latency 중앙값이 모두 목표를 만족했다.

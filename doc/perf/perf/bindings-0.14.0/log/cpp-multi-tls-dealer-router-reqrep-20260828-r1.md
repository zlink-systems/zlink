# C++ paired measurement: Multi / tls / MULTI_DEALER_ROUTER_REQREP

- timestamp: 2026-08-28T08:29:27+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_082927_cpp0140-multi-tls-dealer-router-reqrep-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_083001_cpp0140-multi-tls-dealer-router-reqrep-r1.txt`
- status: 통과(89.6%)
- aggregate throughput ratio: 89.59% (target 85%); aggregate mean-latency ratio median: 1.281x (max 2.0x).
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072; duration=5s; part-count=2.
- META in both reports: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded; native SHA matched before measurement.

| Size | C raw (throughput / mean latency ms) | C++ raw (throughput / mean latency ms) | Throughput / latency ratio |
|---:|---:|---:|---:|
| 64 | 64391.200 / 0.571 | 61854.000 / 0.712 | 96.06% / 1.247x |
| 256 | 63332.000 / 0.583 | 58812.400 / 0.755 | 92.86% / 1.295x |
| 1024 | 55846.200 / 0.642 | 51459.200 / 0.860 | 92.14% / 1.340x |
| 4096 | 54747.600 / 0.675 | 44722.600 / 1.011 | 81.69% / 1.498x |
| 65536 | 9294.800 / 4.914 | 8713.800 / 5.551 | 93.75% / 1.130x |
| 131072 | 7000.000 / 6.833 | 5671.000 / 8.655 | 81.01% / 1.267x |

## 판정 근거

- throughput aggregate와 latency 중앙값이 모두 목표를 만족했다.

# C++ paired measurement: Multi / tls / MULTI_ROUTER_ROUTER_SENDSEND

- timestamp: 2026-08-28T08:30:35+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_083035_cpp0140-multi-tls-router-router-sendsend-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_083110_cpp0140-multi-tls-router-router-sendsend-r1.txt`
- status: 통과(114.1%)
- aggregate throughput ratio: 114.13% (target 85%); aggregate mean-latency ratio median: 0.929x (max 2.0x).
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072; duration=5s; part-count=2.
- META in both reports: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded; native SHA matched before measurement.

| Size | C raw (throughput / mean latency ms) | C++ raw (throughput / mean latency ms) | Throughput / latency ratio |
|---:|---:|---:|---:|
| 64 | 95507.800 / 0.484 | 85284.200 / 0.531 | 89.30% / 1.097x |
| 256 | 95198.200 / 0.486 | 95392.800 / 0.474 | 100.20% / 0.975x |
| 1024 | 87627.600 / 0.525 | 85328.800 / 0.590 | 97.38% / 1.124x |
| 4096 | 76512.000 / 0.610 | 99135.200 / 0.470 | 129.57% / 0.770x |
| 65536 | 12449.400 / 4.701 | 16418.200 / 2.985 | 131.88% / 0.635x |
| 131072 | 6794.000 / 7.251 | 9268.800 / 6.398 | 136.43% / 0.882x |

## 판정 근거

- throughput aggregate와 latency 중앙값이 모두 목표를 만족했다.

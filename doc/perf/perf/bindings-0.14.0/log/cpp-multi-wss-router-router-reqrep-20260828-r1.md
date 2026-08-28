# C++ paired measurement: Multi / wss / MULTI_ROUTER_ROUTER_REQREP

- timestamp: 2026-08-28T08:19:46+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_081946_cpp0140-multi-wss-router-router-reqrep-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_082025_cpp0140-multi-wss-router-router-reqrep-r1.txt`
- status: 미달(79.2%)
- aggregate throughput ratio: 79.18% (target 85%); aggregate mean-latency ratio median: 1.411x (max 2.0x).
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072; duration=5s; part-count=2.
- META in both reports: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded; native SHA matched before measurement.

| Size | C raw (throughput / mean latency ms) | C++ raw (throughput / mean latency ms) | Throughput / latency ratio |
|---:|---:|---:|---:|
| 64 | 63987.800 / 0.579 | 56373.200 / 0.806 | 88.10% / 1.392x |
| 256 | 75348.000 / 0.512 | 52138.800 / 0.877 | 69.20% / 1.713x |
| 1024 | 69402.000 / 0.546 | 43926.400 / 1.034 | 63.29% / 1.894x |
| 4096 | 50895.400 / 0.795 | 40942.800 / 1.137 | 80.44% / 1.430x |
| 65536 | 10115.000 / 4.732 | 8895.600 / 5.541 | 87.94% / 1.171x |
| 131072 | 5351.200 / 9.097 | 4608.000 / 10.699 | 86.11% / 1.176x |

## 판정 근거

- throughput aggregate가 목표에 미달했으며 latency 중앙값은 상한 이내다.
- 개선 후보(미구현): C++ router-routing metadata 변환과 request/reply completion dispatch 비용을 profile로 분리한다.

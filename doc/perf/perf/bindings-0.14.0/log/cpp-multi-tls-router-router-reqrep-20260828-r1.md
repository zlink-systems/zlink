# C++ paired measurement: Multi / tls / MULTI_ROUTER_ROUTER_REQREP

- timestamp: 2026-08-28T08:32:14+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_083214_cpp0140-multi-tls-router-router-reqrep-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_083249_cpp0140-multi-tls-router-router-reqrep-r1.txt`
- status: 미달(79.8%)
- aggregate throughput ratio: 79.79% (target 85%); aggregate mean-latency ratio median: 1.573x (max 2.0x).
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072; duration=5s; part-count=2.
- META in both reports: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded; native SHA matched before measurement.

| Size | C raw (throughput / mean latency ms) | C++ raw (throughput / mean latency ms) | Throughput / latency ratio |
|---:|---:|---:|---:|
| 64 | 77266.800 / 0.472 | 58598.800 / 0.747 | 75.84% / 1.583x |
| 256 | 75399.600 / 0.489 | 54053.800 / 0.821 | 71.69% / 1.679x |
| 1024 | 62266.400 / 0.570 | 48483.400 / 0.907 | 77.86% / 1.591x |
| 4096 | 53509.200 / 0.680 | 42428.200 / 1.063 | 79.29% / 1.563x |
| 65536 | 8599.000 / 5.392 | 7685.600 / 6.251 | 89.38% / 1.159x |
| 131072 | 6742.600 / 7.032 | 5710.600 / 8.591 | 84.69% / 1.222x |

## 판정 근거

- throughput aggregate가 목표에 미달했으며 latency 중앙값은 상한 이내다.
- 개선 후보(미구현): C++ router-routing metadata 변환과 request/reply completion dispatch 비용을 profile로 분리한다.

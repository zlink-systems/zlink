# C++ paired measurement: Multi / wss / MULTI_DEALER_ROUTER_REQREP

- timestamp: 2026-08-28T08:16:55+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_081655_cpp0140-multi-wss-dealer-router-reqrep-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_081732_cpp0140-multi-wss-dealer-router-reqrep-r1.txt`
- status: 미달(72.1%)
- aggregate throughput ratio: 72.13% (target 85%); aggregate mean-latency ratio median: 1.572x (max 2.0x).
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072; duration=5s; part-count=2.
- META in both reports: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded; native SHA matched before measurement.

| Size | C raw (throughput / mean latency ms) | C++ raw (throughput / mean latency ms) | Throughput / latency ratio |
|---:|---:|---:|---:|
| 64 | 79268.400 / 0.496 | 46192.800 / 0.837 | 58.27% / 1.688x |
| 256 | 62275.000 / 0.641 | 52914.200 / 0.856 | 84.97% / 1.335x |
| 1024 | 69096.800 / 0.556 | 44379.200 / 1.021 | 64.23% / 1.836x |
| 4096 | 60368.200 / 0.675 | 38412.600 / 1.209 | 63.63% / 1.791x |
| 65536 | 11108.400 / 4.313 | 7825.600 / 6.285 | 70.45% / 1.457x |
| 131072 | 5324.800 / 9.175 | 4858.600 / 10.138 | 91.24% / 1.105x |

## 판정 근거

- throughput aggregate가 목표에 미달했으며 latency 중앙값은 상한 이내다.
- 개선 후보(미구현): C++ request/reply completion poller와 callback dispatch 비용을 C raw socket 경로와 profile로 대조한다.

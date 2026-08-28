# C++ paired measurement: Multi / ws / MULTI_PUBSUB

- timestamp: 2026-08-28T08:11:07+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_081107_cpp0140-multi-ws-pubsub-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_081141_cpp0140-multi-ws-pubsub-r1.txt`
- status: 통과(150.3%)
- aggregate throughput ratio: 150.35% (target 95%)
- aggregate mean-latency ratio median: 0.908x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 324084.800 | 741162.800 | 228.69% | 1764.032 | 1977.388 | 1.121x | 324084.800/1764.032 | 741162.800/1977.388 |
| 256 | 330266.200 | 684090.000 | 207.13% | 2061.230 | 1679.511 | 0.815x | 330266.200/2061.230 | 684090.000/1679.511 |
| 1024 | 528595.400 | 874479.000 | 165.43% | 1190.901 | 580.343 | 0.487x | 528595.400/1190.901 | 874479.000/580.343 |
| 4096 | 383715.000 | 365436.400 | 95.24% | 282.549 | 336.169 | 1.190x | 383715.000/282.549 | 365436.400/336.169 |
| 65536 | 45611.400 | 40856.800 | 89.58% | 292.004 | 274.227 | 0.939x | 45611.400/292.004 | 40856.800/274.227 |
| 131072 | 20768.200 | 24092.600 | 116.01% | 247.657 | 217.048 | 0.876x | 20768.200/247.657 | 24092.600/217.048 |

## 판정 근거
- throughput aggregate 150.35%와 latency 중앙값 0.908x로 판정했다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

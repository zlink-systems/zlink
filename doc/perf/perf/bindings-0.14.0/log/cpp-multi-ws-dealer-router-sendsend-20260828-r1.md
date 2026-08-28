# C++ paired measurement: Multi / ws / MULTI_DEALER_ROUTER_SENDSEND

- timestamp: 2026-08-28T08:06:09+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_080609_cpp0140-multi-ws-dealer-router-sendsend-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_080642_cpp0140-multi-ws-dealer-router-sendsend-r1.txt`
- status: 통과(96.9%)
- aggregate throughput ratio: 96.85% (target 85%)
- aggregate mean-latency ratio median: 1.108x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 105247.600 | 104122.200 | 98.93% | 0.428 | 0.480 | 1.121x | 105247.600/0.428 | 104122.200/0.480 |
| 256 | 106500.400 | 96014.400 | 90.15% | 0.422 | 0.521 | 1.235x | 106500.400/0.422 | 96014.400/0.521 |
| 1024 | 99961.400 | 84914.400 | 84.95% | 0.451 | 0.589 | 1.306x | 99961.400/0.451 | 84914.400/0.589 |
| 4096 | 88906.800 | 87776.200 | 98.73% | 0.621 | 0.679 | 1.093x | 88906.800/0.621 | 87776.200/0.679 |
| 65536 | 22224.600 | 24462.800 | 110.07% | 2.018 | 2.043 | 1.012x | 22224.600/2.018 | 24462.800/2.043 |
| 131072 | 15537.600 | 15271.600 | 98.29% | 2.987 | 3.270 | 1.095x | 15537.600/2.987 | 15271.600/3.270 |

## 판정 근거
- throughput aggregate 96.85%와 latency 중앙값 1.108x로 판정했다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

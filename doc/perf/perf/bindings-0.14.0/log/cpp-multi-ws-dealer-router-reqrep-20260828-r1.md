# C++ paired measurement: Multi / ws / MULTI_DEALER_ROUTER_REQREP

- timestamp: 2026-08-28T08:07:16+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_080716_cpp0140-multi-ws-dealer-router-reqrep-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_080750_cpp0140-multi-ws-dealer-router-reqrep-r1.txt`
- status: 통과(90.7%)
- aggregate throughput ratio: 90.67% (target 85%)
- aggregate mean-latency ratio median: 1.362x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 72418.800 | 66434.400 | 91.74% | 0.467 | 0.636 | 1.362x | 72418.800/0.467 | 66434.400/0.636 |
| 256 | 67146.800 | 65613.200 | 97.72% | 0.502 | 0.651 | 1.297x | 67146.800/0.502 | 65613.200/0.651 |
| 1024 | 64596.000 | 57523.400 | 89.05% | 0.520 | 0.747 | 1.437x | 64596.000/0.520 | 57523.400/0.747 |
| 4096 | 47858.000 | 40241.400 | 84.09% | 0.672 | 1.082 | 1.610x | 47858.000/0.672 | 40241.400/1.082 |
| 65536 | 19789.600 | 17862.400 | 90.26% | 1.943 | 2.645 | 1.361x | 19789.600/1.943 | 17862.400/2.645 |
| 131072 | 13088.800 | 11936.600 | 91.20% | 3.200 | 4.003 | 1.251x | 13088.800/3.200 | 11936.600/4.003 |

## 판정 근거
- throughput aggregate 90.67%와 latency 중앙값 1.362x로 판정했다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

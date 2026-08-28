# C++ paired measurement: Multi / ws / MULTI_ROUTER_ROUTER_REQREP

- timestamp: 2026-08-28T08:10:00+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_081000_cpp0140-multi-ws-router-router-reqrep-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_081034_cpp0140-multi-ws-router-router-reqrep-r1.txt`
- status: 미달(83.5%)
- aggregate throughput ratio: 83.52% (target 85%)
- aggregate mean-latency ratio median: 1.567x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 83720.400 | 66076.200 | 78.92% | 0.408 | 0.634 | 1.554x | 83720.400/0.408 | 66076.200/0.634 |
| 256 | 80223.200 | 63487.600 | 79.14% | 0.422 | 0.667 | 1.581x | 80223.200/0.422 | 63487.600/0.667 |
| 1024 | 75425.600 | 53384.000 | 70.78% | 0.445 | 0.808 | 1.816x | 75425.600/0.445 | 53384.000/0.808 |
| 4096 | 67808.000 | 47988.600 | 70.77% | 0.514 | 0.912 | 1.774x | 67808.000/0.514 | 47988.600/0.912 |
| 65536 | 16364.800 | 17357.000 | 106.06% | 2.066 | 2.721 | 1.317x | 16364.800/2.066 | 17357.000/2.721 |
| 131072 | 12357.000 | 11794.200 | 95.45% | 3.229 | 4.058 | 1.257x | 12357.000/3.229 | 11794.200/4.058 |

## 판정 근거
- throughput aggregate 83.52%와 latency 중앙값 1.567x로 판정했다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

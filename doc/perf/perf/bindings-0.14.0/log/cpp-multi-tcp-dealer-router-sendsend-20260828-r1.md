# C++ paired measurement: Multi / tcp / MULTI_DEALER_ROUTER_SENDSEND

- timestamp: 2026-08-28T07:51:07+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_075107_cpp0140-multi-tcp-dealer-router-sendsend-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_075150_cpp0140-multi-tcp-dealer-router-sendsend-r1.txt`
- status: 통과(88.9%)
- aggregate throughput ratio: 88.86% (target 85%)
- aggregate mean-latency ratio median: 1.370x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 147469.200 | 126367.000 | 85.69% | 0.300 | 0.396 | 1.320x | 147469.200/0.300 | 126367.000/0.396 |
| 256 | 138276.800 | 119952.800 | 86.75% | 0.318 | 0.499 | 1.569x | 138276.800/0.318 | 119952.800/0.499 |
| 1024 | 130601.800 | 114296.200 | 87.52% | 0.337 | 0.437 | 1.297x | 130601.800/0.337 | 114296.200/0.437 |
| 4096 | 121896.400 | 112068.800 | 91.94% | 0.424 | 0.446 | 1.052x | 121896.400/0.424 | 112068.800/0.446 |
| 65536 | 30910.600 | 27961.200 | 90.46% | 1.247 | 1.787 | 1.433x | 30910.600/1.247 | 27961.200/1.787 |
| 131072 | 18225.600 | 16552.800 | 90.82% | 2.125 | 3.018 | 1.420x | 18225.600/2.125 | 16552.800/3.018 |

## 판정 근거
- throughput aggregate 88.86%와 latency 중앙값 1.370x로 판정했다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

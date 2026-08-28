# C++ paired measurement: Multi / ws / MULTI_ROUTER_ROUTER_SENDSEND

- timestamp: 2026-08-28T08:08:23+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_080823_cpp0140-multi-ws-router-router-sendsend-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_080857_cpp0140-multi-ws-router-router-sendsend-r1.txt`
- status: 통과(118.4%)
- aggregate throughput ratio: 118.40% (target 85%)
- aggregate mean-latency ratio median: 0.810x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 105270.000 | 105513.000 | 100.23% | 0.433 | 0.424 | 0.979x | 105270.000/0.433 | 105513.000/0.424 |
| 256 | 96703.600 | 121988.400 | 126.15% | 0.468 | 0.367 | 0.784x | 96703.600/0.468 | 121988.400/0.367 |
| 1024 | 101029.600 | 121263.000 | 120.03% | 0.451 | 0.371 | 0.823x | 101029.600/0.451 | 121263.000/0.371 |
| 4096 | 93006.200 | 107753.000 | 115.86% | 0.580 | 0.431 | 0.743x | 93006.200/0.580 | 107753.000/0.431 |
| 65536 | 23062.600 | 29105.600 | 126.20% | 1.940 | 1.551 | 0.799x | 23062.600/1.940 | 29105.600/1.551 |
| 131072 | 14530.000 | 17720.400 | 121.96% | 3.248 | 2.666 | 0.821x | 14530.000/3.248 | 17720.400/2.666 |

## 판정 근거
- throughput aggregate 118.40%와 latency 중앙값 0.810x로 판정했다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

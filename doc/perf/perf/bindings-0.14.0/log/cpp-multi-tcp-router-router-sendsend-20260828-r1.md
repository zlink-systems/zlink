# C++ paired measurement: Multi / tcp / MULTI_ROUTER_ROUTER_SENDSEND

- timestamp: 2026-08-28T07:55:35+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_075535_cpp0140-multi-tcp-router-router-sendsend-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_075609_cpp0140-multi-tcp-router-router-sendsend-r1.txt`
- status: 통과(95.4%)
- aggregate throughput ratio: 95.37% (target 85%)
- aggregate mean-latency ratio median: 0.983x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 155487.600 | 128384.000 | 82.57% | 0.288 | 0.336 | 1.167x | 155487.600/0.288 | 128384.000/0.336 |
| 256 | 145117.800 | 128257.800 | 88.38% | 0.306 | 0.335 | 1.095x | 145117.800/0.306 | 128257.800/0.335 |
| 1024 | 135176.800 | 131264.600 | 97.11% | 0.330 | 0.332 | 1.006x | 135176.800/0.330 | 131264.600/0.332 |
| 4096 | 127132.200 | 124958.200 | 98.29% | 0.427 | 0.350 | 0.820x | 127132.200/0.427 | 124958.200/0.350 |
| 65536 | 31285.000 | 31427.800 | 100.46% | 1.240 | 1.191 | 0.960x | 31285.000/1.240 | 31427.800/1.191 |
| 131072 | 19012.800 | 20039.400 | 105.40% | 2.072 | 1.851 | 0.893x | 19012.800/2.072 | 20039.400/1.851 |

## 판정 근거
- throughput aggregate 95.37%와 latency 중앙값 0.983x로 판정했다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

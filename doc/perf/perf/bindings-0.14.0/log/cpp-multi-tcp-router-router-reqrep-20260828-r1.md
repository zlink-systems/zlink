# C++ paired measurement: Multi / tcp / MULTI_ROUTER_ROUTER_REQREP

- timestamp: 2026-08-28T07:57:13+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_075713_cpp0140-multi-tcp-router-router-reqrep-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_075746_cpp0140-multi-tcp-router-router-reqrep-r1.txt`
- status: 통과(87.3%)
- aggregate throughput ratio: 87.35% (target 85%)
- aggregate mean-latency ratio median: 1.546x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 84703.200 | 74975.400 | 88.52% | 0.366 | 0.540 | 1.475x | 84703.200/0.366 | 74975.400/0.540 |
| 256 | 78043.000 | 69200.600 | 88.67% | 0.393 | 0.591 | 1.504x | 78043.000/0.393 | 69200.600/0.591 |
| 1024 | 74358.200 | 61384.000 | 82.55% | 0.403 | 0.690 | 1.712x | 74358.200/0.403 | 61384.000/0.690 |
| 4096 | 57459.800 | 47006.200 | 81.81% | 0.539 | 0.892 | 1.655x | 57459.800/0.539 | 47006.200/0.892 |
| 65536 | 25639.800 | 21383.800 | 83.40% | 1.329 | 2.112 | 1.589x | 25639.800/1.329 | 21383.800/2.112 |
| 131072 | 14636.000 | 14512.000 | 99.15% | 2.101 | 3.123 | 1.486x | 14636.000/2.101 | 14512.000/3.123 |

## 판정 근거
- throughput aggregate 87.35%와 latency 중앙값 1.546x로 판정했다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

# C++ paired measurement: Multi / ws / MULTI_STREAM

- timestamp: 2026-08-28T08:12:16+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_081216_cpp0140-multi-ws-stream-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_081239_cpp0140-multi-ws-stream-r1.txt`
- status: 통과(101.5%)
- aggregate throughput ratio: 101.52% (target 95%)
- aggregate mean-latency ratio median: 0.976x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,65536.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 243261.800 | 253810.200 | 104.34% | 0.415 | 0.401 | 0.966x | 243261.800/0.415 | 253810.200/0.401 |
| 256 | 248214.600 | 244312.000 | 98.43% | 0.406 | 0.412 | 1.015x | 248214.600/0.406 | 244312.000/0.412 |
| 1024 | 231858.000 | 234293.800 | 101.05% | 0.436 | 0.430 | 0.986x | 231858.000/0.436 | 234293.800/0.430 |
| 65536 | 16206.200 | 16571.600 | 102.25% | 7.303 | 6.036 | 0.827x | 16206.200/7.303 | 16571.600/6.036 |

## 판정 근거
- throughput aggregate 101.52%와 latency 중앙값 0.976x로 판정했다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

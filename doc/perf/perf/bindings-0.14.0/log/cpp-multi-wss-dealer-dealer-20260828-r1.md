# C++ paired measurement: Multi / wss / MULTI_DEALER_DEALER

- timestamp: 2026-08-28T08:14:59+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_081459_cpp0140-multi-wss-dealer-dealer-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_081537_cpp0140-multi-wss-dealer-dealer-r1.txt`
- status: 미달(57.2%)
- aggregate throughput ratio: 57.21% (target 95%); aggregate mean-latency ratio median: 1.931x (max 2.0x).
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072; duration=5s; part-count=2.
- runtime SHA: Core and C++ native runtime `a6f7a7fb727b7e1e05cc9a7f088376af5a5c34e0fcbc34bc2601b9674b077777` matched before measurement.
- META in both reports: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime=`/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`.

| Size | C raw (throughput / mean latency ms) | C++ raw (throughput / mean latency ms) | Throughput / latency ratio |
|---:|---:|---:|---:|
| 64 | 792874.400 / 498.406 | 335000.000 / 2998.140 | 42.25% / 6.015x |
| 256 | 891575.600 / 2.694 | 303160.000 / 1622.066 | 34.00% / 602.103x |
| 1024 | 529305.400 / 997.650 | 293160.000 / 950.824 | 55.39% / 0.953x |
| 4096 | 222471.400 / 60.559 | 162673.000 / 176.112 | 73.12% / 2.908x |
| 65536 | 27202.000 / 269.398 | 17646.400 / 180.563 | 64.87% / 0.670x |
| 131072 | 16342.200 / 229.148 | 12033.800 / 76.856 | 73.64% / 0.335x |

## 판정 근거

- throughput aggregate가 목표에 미달했으며 latency 중앙값은 상한 이내다.
- 개선 후보(미구현): 64~1024B C++ multi DEALER_DEALER의 public send/receive dispatch 비용을 C reference와 profile로 대조한다.

# C++ paired measurement: Multi / wss / MULTI_PUBSUB

- timestamp: 2026-08-28T08:21:01+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_082101_cpp0140-multi-wss-pubsub-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_082135_cpp0140-multi-wss-pubsub-r1.txt`
- status: 통과(143.5%)
- aggregate throughput ratio: 143.51% (target 95%); aggregate mean-latency ratio median: 0.961x (max 2.0x).
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072; duration=5s; part-count=2.
- META in both reports: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded; native SHA matched before measurement.

| Size | C raw (throughput / mean latency ms) | C++ raw (throughput / mean latency ms) | Throughput / latency ratio |
|---:|---:|---:|---:|
| 64 | 371878.400 / 1469.221 | 648197.000 / 1670.740 | 174.30% / 1.137x |
| 256 | 361342.600 / 1371.121 | 830691.000 / 1652.639 | 229.89% / 1.205x |
| 1024 | 502758.000 / 925.146 | 679775.200 / 628.877 | 135.21% / 0.680x |
| 4096 | 220822.000 / 395.638 | 223326.000 / 390.335 | 101.13% / 0.987x |
| 65536 | 20956.600 / 282.635 | 24703.200 / 264.493 | 117.88% / 0.936x |
| 131072 | 13032.800 / 280.988 | 13380.200 / 254.053 | 102.67% / 0.904x |

## 판정 근거

- throughput aggregate와 latency 중앙값이 모두 목표를 만족했다.

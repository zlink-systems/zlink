# C++ paired measurement: Multi / tls / MULTI_DEALER_DEALER

- timestamp: 2026-08-28T08:26:58+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_082658_cpp0140-multi-tls-dealer-dealer-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_082733_cpp0140-multi-tls-dealer-dealer-r1.txt`
- status: 미달(58.1%)
- aggregate throughput ratio: 58.08% (target 95%); aggregate mean-latency ratio median: 1.123x (max 2.0x).
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072; duration=5s; part-count=2.
- runtime SHA: Core and C++ native runtime `a6f7a7fb727b7e1e05cc9a7f088376af5a5c34e0fcbc34bc2601b9674b077777` matched before measurement.
- META in both reports: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime=`/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`.

| Size | C raw (throughput / mean latency ms) | C++ raw (throughput / mean latency ms) | Throughput / latency ratio |
|---:|---:|---:|---:|
| 64 | 621458.200 / 855.296 | 299280.000 / 3900.856 | 48.16% / 4.561x |
| 256 | 903037.600 / 2.949 | 305980.000 / 1970.724 | 33.88% / 668.269x |
| 1024 | 593356.600 / 810.962 | 291200.000 / 957.454 | 49.08% / 1.181x |
| 4096 | 288460.600 / 694.524 | 247860.000 / 740.316 | 85.93% / 1.066x |
| 65536 | 39914.200 / 204.900 | 25072.800 / 100.887 | 62.82% / 0.492x |
| 131072 | 22258.400 / 188.233 | 15272.400 / 126.408 | 68.61% / 0.672x |

## 판정 근거

- throughput aggregate가 목표에 미달했으며 latency 중앙값은 상한 이내다.
- 개선 후보(미구현): 64~1024B C++ multi DEALER_DEALER의 public send/receive dispatch 비용을 C reference와 profile로 대조한다.

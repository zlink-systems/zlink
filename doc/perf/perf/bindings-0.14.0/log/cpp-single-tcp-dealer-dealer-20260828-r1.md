# C++ paired measurement: Single / tcp / DEALER_DEALER

- timestamp: 2026-08-28T06:42:09+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_064209_cpp0140-single-tcp-dealer-dealer-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_064241_cpp0140-single-tcp-dealer-dealer-r1.txt`
- status: 미달(83.6%)
- aggregate throughput ratio: 83.58% (target 95%)
- aggregate mean-latency ratio: 0.957x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 943904.400 | 727308.800 | 77.05% | 73.645 | 7.922 | 0.108x | 943904.4/73.645 | 727308.8/7.922 |
| 256 | 868895.600 | 666515.400 | 76.71% | 3.277 | 0.182 | 0.056x | 868895.6/3.277 | 666515.4/0.182 |
| 1024 | 871880.000 | 668283.600 | 76.65% | 1.247 | 2.645 | 2.121x | 871880.0/1.247 | 668283.6/2.645 |
| 65536 | 37575.000 | 32021.600 | 85.22% | 0.280 | 0.304 | 1.086x | 37575.0/0.280 | 32021.6/0.304 |
| 131072 | 27246.200 | 24782.400 | 90.96% | 0.199 | 0.264 | 1.327x | 27246.2/0.199 | 24782.4/0.264 |
| 262144 | 16922.400 | 16055.800 | 94.88% | 0.189 | 0.198 | 1.048x | 16922.4/0.189 | 16055.8/0.198 |

## 판정 근거

- Throughput aggregate 83.58%가 95% 목표에 미달했고 latency aggregate는 상한 이내다.
- 개선 후보(미구현): C++ `DEALER_DEALER` public receive/send dispatch와 C reference의 hot path 비용을 profiling으로 비교한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

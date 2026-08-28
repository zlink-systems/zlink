# C++ paired measurement: Single / tcp / DEALER_ROUTER

- timestamp: 2026-08-28T06:43:12+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_064312_cpp0140-single-tcp-dealer-router-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_064344_cpp0140-single-tcp-dealer-router-r1.txt`
- status: 미달(87.9%)
- aggregate throughput ratio: 87.91% (target 85%)
- aggregate mean-latency ratio: 3.590x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 807233.400 | 683045.200 | 84.62% | 25.750 | 45.625 | 1.772x | 807233.4/25.750 | 683045.2/45.625 |
| 256 | 814301.800 | 649457.200 | 79.76% | 0.348 | 0.472 | 1.356x | 814301.8/0.348 | 649457.2/0.472 |
| 1024 | 775383.600 | 639712.800 | 82.50% | 0.978 | 14.852 | 15.186x | 775383.6/0.978 | 639712.8/14.852 |
| 65536 | 35841.400 | 31712.400 | 88.48% | 0.250 | 0.302 | 1.208x | 35841.4/0.250 | 31712.4/0.302 |
| 131072 | 26119.800 | 24233.400 | 92.78% | 0.239 | 0.245 | 1.025x | 26119.8/0.239 | 24233.4/0.245 |
| 262144 | 15746.600 | 15641.400 | 99.33% | 0.206 | 0.205 | 0.995x | 15746.6/0.206 | 15641.4/0.205 |

## 판정 근거

- Throughput aggregate는 목표를 넘지만 mean-latency aggregate 3.590x가 2.0x 상한을 넘었다.
- 개선 후보(미구현): 1 KiB routed receive의 C++ callback/poller dispatch와 latency sample 수집 경계를 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

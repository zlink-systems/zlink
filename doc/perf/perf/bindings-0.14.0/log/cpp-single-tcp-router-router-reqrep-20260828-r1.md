# C++ paired measurement: Single / tcp / ROUTER_ROUTER_REQREP

- timestamp: 2026-08-28T06:46:20+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_064620_cpp0140-single-tcp-router-router-reqrep-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_064651_cpp0140-single-tcp-router-router-reqrep-r1.txt`
- status: 미달(66.0%)
- aggregate throughput ratio: 65.98% (target 85%)
- aggregate mean-latency ratio: 2.001x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 183315.000 | 108924.800 | 59.42% | 0.249 | 0.474 | 1.904x | 183315.0/0.249 | 108924.8/0.474 |
| 256 | 166818.200 | 93255.800 | 55.90% | 0.220 | 0.570 | 2.591x | 166818.2/0.220 | 93255.8/0.570 |
| 1024 | 155892.600 | 65907.000 | 42.28% | 0.240 | 0.917 | 3.821x | 155892.6/0.240 | 65907.0/0.917 |
| 65536 | 18410.800 | 14035.400 | 76.23% | 0.647 | 0.837 | 1.294x | 18410.8/0.647 | 14035.4/0.837 |
| 131072 | 12715.800 | 10666.600 | 83.88% | 0.469 | 0.545 | 1.162x | 12715.8/0.469 | 10666.6/0.545 |
| 262144 | 7620.400 | 5957.000 | 78.17% | 0.390 | 0.481 | 1.233x | 7620.4/0.390 | 5957.0/0.481 |

## 판정 근거

- Throughput aggregate 65.98%가 85% 목표에 미달했다; mean-latency aggregate도 2.001x로 상한을 아주 작게 넘었다.
- 개선 후보(미구현): public request/reply route metadata 변환·callback completion·reply dispatch의 C++ 비용을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

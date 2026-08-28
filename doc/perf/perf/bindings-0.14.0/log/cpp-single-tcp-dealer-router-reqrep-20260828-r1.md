# C++ paired measurement: Single / tcp / DEALER_ROUTER_REQREP

- timestamp: 2026-08-28T06:44:15+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_064415_cpp0140-single-tcp-dealer-router-reqrep-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_064447_cpp0140-single-tcp-dealer-router-reqrep-r1.txt`
- status: 미달(64.5%)
- aggregate throughput ratio: 64.45% (target 85%)
- aggregate mean-latency ratio: 2.078x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 182369.800 | 108541.200 | 59.52% | 0.219 | 0.436 | 1.991x | 182369.8/0.219 | 108541.2/0.436 |
| 256 | 178126.200 | 95127.600 | 53.40% | 0.230 | 0.571 | 2.483x | 178126.2/0.230 | 95127.6/0.571 |
| 1024 | 165805.000 | 63910.200 | 38.55% | 0.222 | 0.946 | 4.261x | 165805.0/0.222 | 63910.2/0.946 |
| 65536 | 18688.600 | 14521.800 | 77.70% | 0.638 | 0.809 | 1.268x | 18688.6/0.638 | 14521.8/0.809 |
| 131072 | 13089.200 | 10408.400 | 79.52% | 0.455 | 0.558 | 1.226x | 13089.2/0.455 | 10408.4/0.558 |
| 262144 | 7922.200 | 6181.600 | 78.03% | 0.437 | 0.541 | 1.238x | 7922.2/0.437 | 6181.6/0.541 |

## 판정 근거

- Throughput aggregate 64.45%와 mean-latency aggregate 2.078x가 각각 목표와 상한을 충족하지 못했다.
- 개선 후보(미구현): public request/reply completion, routing metadata 변환, reply dispatch의 allocation/copy 비용을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

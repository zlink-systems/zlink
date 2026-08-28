# C++ paired measurement: Single / tcp / ROUTER_ROUTER

- timestamp: 2026-08-28T06:45:18+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_064518_cpp0140-single-tcp-router-router-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_064549_cpp0140-single-tcp-router-router-r1.txt`
- status: 미달(87.4%)
- aggregate throughput ratio: 87.37% (target 85%)
- aggregate mean-latency ratio: 13.757x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 687384.400 | 624017.800 | 90.78% | 0.237 | 17.485 | 73.776x | 687384.4/0.237 | 624017.8/17.485 |
| 256 | 726202.600 | 592165.800 | 81.54% | 0.382 | 1.787 | 4.678x | 726202.6/0.382 | 592165.8/1.787 |
| 1024 | 712407.400 | 592799.400 | 83.21% | 9.376 | 6.908 | 0.737x | 712407.4/9.376 | 592799.4/6.908 |
| 65536 | 35794.000 | 33162.600 | 92.65% | 0.252 | 0.279 | 1.107x | 35794.0/0.252 | 33162.6/0.279 |
| 131072 | 24947.200 | 22314.600 | 89.45% | 0.219 | 0.242 | 1.105x | 24947.2/0.219 | 22314.6/0.242 |
| 262144 | 15497.000 | 13418.600 | 86.59% | 0.206 | 0.234 | 1.136x | 15497.0/0.206 | 13418.6/0.234 |

## 판정 근거

- Throughput aggregate는 목표를 넘지만 mean-latency aggregate 13.757x가 2.0x 상한을 넘었다.
- 개선 후보(미구현): 64B/256B routed receiver의 C++ callback/poller and latency-sampling queue residence를 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

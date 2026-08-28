# C++ paired measurement: Single / ws / DEALER_ROUTER_REQREP

- timestamp: 2026-08-28T06:56:21+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_065621_cpp0140-single-ws-dealer-router-reqrep-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_065652_cpp0140-single-ws-dealer-router-reqrep-r1.txt`
- status: 미달(60.2%)
- aggregate throughput ratio: 60.24% (target 85%)
- aggregate mean-latency ratio: 2.575x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 181051.800 | 40040.600 | 22.12% | 0.277 | 1.826 | 6.592x | 181051.800/0.277 | 40040.600/1.826 |
| 256 | 99746.000 | 34272.400 | 34.36% | 0.599 | 1.835 | 3.063x | 99746.000/0.599 | 34272.400/1.835 |
| 1024 | 59722.200 | 24811.200 | 41.54% | 1.047 | 2.557 | 2.442x | 59722.200/1.047 | 24811.200/2.557 |
| 65536 | 10498.800 | 9056.000 | 86.26% | 1.139 | 1.305 | 1.146x | 10498.800/1.139 | 9056.000/1.305 |
| 131072 | 7751.400 | 6711.600 | 86.59% | 0.771 | 0.875 | 1.135x | 7751.400/0.771 | 6711.600/0.875 |
| 262144 | 4949.000 | 4483.800 | 90.60% | 0.603 | 0.648 | 1.075x | 4949.000/0.603 | 4483.800/0.648 |

## 판정 근거

- throughput aggregate 60.24%와 mean-latency aggregate 2.575x를 target 85% / max 2.0x와 비교해 `미달(60.2%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

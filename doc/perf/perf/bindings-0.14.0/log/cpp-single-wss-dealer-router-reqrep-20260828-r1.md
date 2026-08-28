# C++ paired measurement: Single / wss / DEALER_ROUTER_REQREP

- timestamp: 2026-08-28T07:06:10+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_070610_cpp0140-single-wss-dealer-router-reqrep-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_070641_cpp0140-single-wss-dealer-router-reqrep-r1.txt`
- status: 미달(66.2%)
- aggregate throughput ratio: 66.24% (target 85%)
- aggregate mean-latency ratio: 1.849x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 163495.800 | 59503.400 | 36.39% | 0.327 | 1.028 | 3.144x |
| 256 | 92429.800 | 39027.000 | 42.22% | 0.655 | 1.603 | 2.447x |
| 1024 | 51187.200 | 22699.400 | 44.35% | 1.420 | 3.216 | 2.265x |
| 65536 | 4485.800 | 4046.200 | 90.20% | 2.672 | 2.947 | 1.103x |
| 131072 | 2758.200 | 2549.400 | 92.43% | 2.172 | 2.325 | 1.070x |
| 262144 | 1538.200 | 1412.800 | 91.85% | 1.947 | 2.073 | 1.065x |

## 판정 근거

- throughput aggregate 66.24%와 mean-latency aggregate 1.849x를 target 85% / max 2.0x와 비교해 `미달(66.2%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

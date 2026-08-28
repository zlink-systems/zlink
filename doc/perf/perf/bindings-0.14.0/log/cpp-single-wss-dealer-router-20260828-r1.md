# C++ paired measurement: Single / wss / DEALER_ROUTER

- timestamp: 2026-08-28T07:05:07+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_070507_cpp0140-single-wss-dealer-router-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_070538_cpp0140-single-wss-dealer-router-r1.txt`
- status: 통과(93.4%)
- aggregate throughput ratio: 93.36% (target 85%)
- aggregate mean-latency ratio: 1.071x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 862516.200 | 678450.800 | 78.66% | 104.408 | 136.121 | 1.304x |
| 256 | 737816.400 | 662168.800 | 89.75% | 47.662 | 53.015 | 1.112x |
| 1024 | 256162.400 | 261232.400 | 101.98% | 31.463 | 28.995 | 0.922x |
| 65536 | 10065.000 | 9818.000 | 97.55% | 12.815 | 13.791 | 1.076x |
| 131072 | 6107.200 | 5817.800 | 95.26% | 9.838 | 10.241 | 1.041x |
| 262144 | 3322.400 | 3222.400 | 96.99% | 9.123 | 8.844 | 0.969x |

## 판정 근거

- throughput aggregate 93.36%와 mean-latency aggregate 1.071x를 target 85% / max 2.0x와 비교해 `통과(93.4%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

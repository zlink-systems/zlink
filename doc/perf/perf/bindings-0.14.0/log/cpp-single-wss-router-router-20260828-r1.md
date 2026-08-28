# C++ paired measurement: Single / wss / ROUTER_ROUTER

- timestamp: 2026-08-28T07:07:12+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_070712_cpp0140-single-wss-router-router-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_070743_cpp0140-single-wss-router-router-r1.txt`
- status: 통과(89.4%)
- aggregate throughput ratio: 89.37% (target 85%)
- aggregate mean-latency ratio: 1.136x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 830787.200 | 655597.400 | 78.91% | 100.727 | 143.457 | 1.424x |
| 256 | 759069.000 | 627742.800 | 82.70% | 35.447 | 43.928 | 1.239x |
| 1024 | 379946.000 | 345803.000 | 91.01% | 24.970 | 23.443 | 0.939x |
| 65536 | 10467.400 | 10063.000 | 96.14% | 15.750 | 16.289 | 1.034x |
| 131072 | 5902.800 | 5468.400 | 92.64% | 12.097 | 13.036 | 1.078x |
| 262144 | 3224.600 | 3056.600 | 94.79% | 11.158 | 12.273 | 1.100x |

## 판정 근거

- throughput aggregate 89.37%와 mean-latency aggregate 1.136x를 target 85% / max 2.0x와 비교해 `통과(89.4%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

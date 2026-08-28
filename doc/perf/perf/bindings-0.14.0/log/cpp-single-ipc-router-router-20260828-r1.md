# C++ paired measurement: Single / ipc / ROUTER_ROUTER

- timestamp: 2026-08-28T07:32:16+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_073216_cpp0140-single-ipc-router-router-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_073247_cpp0140-single-ipc-router-router-r1.txt`
- status: 미달(87.9%)
- aggregate throughput ratio: 87.90% (target 85%)
- aggregate mean-latency ratio: 9.405x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 810715.200 | 756064.200 | 93.26% | 0.676 | 33.401 | 49.410x |
| 256 | 739082.800 | 589964.600 | 79.82% | 0.186 | 0.542 | 2.914x |
| 1024 | 721786.400 | 594516.800 | 82.37% | 0.523 | 0.395 | 0.755x |
| 65536 | 38893.600 | 34693.000 | 89.20% | 0.232 | 0.275 | 1.185x |
| 131072 | 28016.200 | 25603.800 | 91.39% | 0.195 | 0.209 | 1.072x |
| 262144 | 17207.400 | 15718.200 | 91.35% | 0.183 | 0.200 | 1.093x |

## 판정 근거

- throughput aggregate 87.90%와 mean-latency aggregate 9.405x를 target 85% / max 2.0x와 비교해 `미달(87.9%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

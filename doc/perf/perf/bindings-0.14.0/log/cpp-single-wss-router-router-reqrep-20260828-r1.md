# C++ paired measurement: Single / wss / ROUTER_ROUTER_REQREP

- timestamp: 2026-08-28T07:08:15+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_070815_cpp0140-single-wss-router-router-reqrep-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_070846_cpp0140-single-wss-router-router-reqrep-r1.txt`
- status: 미달(72.3%)
- aggregate throughput ratio: 72.26% (target 85%)
- aggregate mean-latency ratio: 1.924x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 152919.200 | 57452.000 | 37.57% | 0.336 | 1.063 | 3.164x |
| 256 | 94694.400 | 38481.600 | 40.64% | 0.601 | 1.627 | 2.707x |
| 1024 | 60066.600 | 22641.400 | 37.69% | 0.993 | 2.804 | 2.824x |
| 65536 | 3503.200 | 4046.600 | 115.51% | 3.948 | 2.948 | 0.747x |
| 131072 | 2295.000 | 2420.400 | 105.46% | 2.610 | 2.825 | 1.082x |
| 262144 | 1537.000 | 1486.200 | 96.69% | 1.948 | 1.990 | 1.022x |

## 판정 근거

- throughput aggregate 72.26%와 mean-latency aggregate 1.924x를 target 85% / max 2.0x와 비교해 `미달(72.3%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

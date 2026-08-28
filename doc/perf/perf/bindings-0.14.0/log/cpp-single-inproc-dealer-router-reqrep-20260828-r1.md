# C++ paired measurement: Single / inproc / DEALER_ROUTER_REQREP

- timestamp: 2026-08-28T07:22:59+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_072259_cpp0140-single-inproc-dealer-router-reqrep-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_072331_cpp0140-single-inproc-dealer-router-reqrep-r1.txt`
- status: 미달(38.3%)
- aggregate throughput ratio: 38.32% (target 85%)
- aggregate mean-latency ratio: 1.088x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 225903.200 | 96626.400 | 42.77% | 0.202 | 0.096 | 0.475x |
| 256 | 223183.800 | 95844.200 | 42.94% | 0.158 | 0.072 | 0.456x |
| 1024 | 215453.600 | 92981.600 | 43.16% | 0.157 | 0.073 | 0.465x |
| 65536 | 114957.600 | 39531.800 | 34.39% | 0.057 | 0.073 | 1.281x |
| 131072 | 80441.000 | 29933.200 | 37.21% | 0.043 | 0.077 | 1.791x |
| 262144 | 44364.000 | 13072.800 | 29.47% | 0.048 | 0.099 | 2.062x |

## 판정 근거

- throughput aggregate 38.32%와 mean-latency aggregate 1.088x를 target 85% / max 2.0x와 비교해 `미달(38.3%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

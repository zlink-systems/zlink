# C++ paired measurement: Single / inproc / ROUTER_ROUTER_REQREP

- timestamp: 2026-08-28T07:25:05+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_072505_cpp0140-single-inproc-router-router-reqrep-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_072536_cpp0140-single-inproc-router-router-reqrep-r1.txt`
- status: 미달(39.5%)
- aggregate throughput ratio: 39.48% (target 85%)
- aggregate mean-latency ratio: 1.038x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 214603.600 | 94493.600 | 44.03% | 0.155 | 0.073 | 0.471x |
| 256 | 205324.400 | 87662.000 | 42.69% | 0.167 | 0.078 | 0.467x |
| 1024 | 188273.000 | 85898.400 | 45.62% | 0.173 | 0.082 | 0.474x |
| 65536 | 111709.000 | 39823.000 | 35.65% | 0.058 | 0.072 | 1.241x |
| 131072 | 74172.800 | 29027.000 | 39.13% | 0.049 | 0.078 | 1.592x |
| 262144 | 42115.000 | 12535.600 | 29.77% | 0.051 | 0.101 | 1.980x |

## 판정 근거

- throughput aggregate 39.48%와 mean-latency aggregate 1.038x를 target 85% / max 2.0x와 비교해 `미달(39.5%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

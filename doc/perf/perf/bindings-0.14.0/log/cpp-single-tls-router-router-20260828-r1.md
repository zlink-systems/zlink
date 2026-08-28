# C++ paired measurement: Single / tls / ROUTER_ROUTER

- timestamp: 2026-08-28T07:15:55+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_071555_cpp0140-single-tls-router-router-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_071626_cpp0140-single-tls-router-router-r1.txt`
- status: 통과(92.4%)
- aggregate throughput ratio: 92.36% (target 85%)
- aggregate mean-latency ratio: 1.415x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 809841.000 | 638759.800 | 78.87% | 107.577 | 169.241 | 1.573x |
| 256 | 776734.200 | 649587.600 | 83.63% | 33.921 | 2.897 | 0.085x |
| 1024 | 502061.600 | 487306.200 | 97.06% | 19.100 | 16.436 | 0.861x |
| 65536 | 13340.600 | 13185.400 | 98.84% | 9.233 | 9.297 | 1.007x |
| 131072 | 7425.800 | 7437.800 | 100.16% | 7.910 | 8.036 | 1.016x |
| 262144 | 3864.400 | 3694.400 | 95.60% | 2.490 | 9.825 | 3.946x |

## 판정 근거

- throughput aggregate 92.36%와 mean-latency aggregate 1.415x를 target 85% / max 2.0x와 비교해 `통과(92.4%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

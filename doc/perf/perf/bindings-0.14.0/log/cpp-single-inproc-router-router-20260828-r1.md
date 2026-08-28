# C++ paired measurement: Single / inproc / ROUTER_ROUTER

- timestamp: 2026-08-28T07:24:02+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_072402_cpp0140-single-inproc-router-router-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_072433_cpp0140-single-inproc-router-router-r1.txt`
- status: 미달(63.2%)
- aggregate throughput ratio: 63.23% (target 85%)
- aggregate mean-latency ratio: 1.976x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 841970.600 | 732462.800 | 86.99% | 5.322 | 6.127 | 1.151x |
| 256 | 791325.200 | 702325.200 | 88.75% | 3.013 | 3.795 | 1.260x |
| 1024 | 757334.600 | 687146.600 | 90.73% | 0.939 | 1.050 | 1.118x |
| 65536 | 361863.800 | 102728.200 | 28.39% | 0.007 | 0.024 | 3.429x |
| 131072 | 161807.400 | 64107.400 | 39.62% | 0.012 | 0.031 | 2.583x |
| 262144 | 77317.400 | 34701.800 | 44.88% | 0.019 | 0.044 | 2.316x |

## 판정 근거

- throughput aggregate 63.23%와 mean-latency aggregate 1.976x를 target 85% / max 2.0x와 비교해 `미달(63.2%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

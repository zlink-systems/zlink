# C++ paired measurement: Single / inproc / DEALER_DEALER

- timestamp: 2026-08-28T07:20:54+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_072054_cpp0140-single-inproc-dealer-dealer-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_072125_cpp0140-single-inproc-dealer-dealer-r1.txt`
- status: 미달(63.1%)
- aggregate throughput ratio: 63.09% (target 95%)
- aggregate mean-latency ratio: 1.530x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 906692.600 | 756674.400 | 83.45% | 4.940 | 5.914 | 1.197x |
| 256 | 824896.000 | 694550.400 | 84.20% | 2.698 | 3.206 | 1.188x |
| 1024 | 796449.000 | 696487.600 | 87.45% | 0.904 | 1.021 | 1.129x |
| 65536 | 343583.400 | 86660.000 | 25.22% | 0.009 | 0.022 | 2.444x |
| 131072 | 194272.200 | 110739.800 | 57.00% | 0.011 | 0.016 | 1.455x |
| 262144 | 70608.000 | 29081.800 | 41.19% | 0.030 | 0.053 | 1.767x |

## 판정 근거

- throughput aggregate 63.09%와 mean-latency aggregate 1.530x를 target 95% / max 2.0x와 비교해 `미달(63.1%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

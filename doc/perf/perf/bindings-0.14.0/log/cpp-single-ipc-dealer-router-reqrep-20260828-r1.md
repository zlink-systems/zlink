# C++ paired measurement: Single / ipc / DEALER_ROUTER_REQREP

- timestamp: 2026-08-28T07:31:13+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_073113_cpp0140-single-ipc-dealer-router-reqrep-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_073145_cpp0140-single-ipc-dealer-router-reqrep-r1.txt`
- status: 미달(65.7%)
- aggregate throughput ratio: 65.71% (target 85%)
- aggregate mean-latency ratio: 1.957x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 186681.400 | 105335.600 | 56.43% | 0.205 | 0.360 | 1.756x |
| 256 | 182480.200 | 96717.600 | 53.00% | 0.293 | 0.570 | 1.945x |
| 1024 | 172538.800 | 65719.200 | 38.09% | 0.204 | 0.914 | 4.480x |
| 65536 | 19075.600 | 14872.000 | 77.96% | 0.624 | 0.789 | 1.264x |
| 131072 | 13543.200 | 11463.600 | 84.64% | 0.439 | 0.505 | 1.150x |
| 262144 | 7756.800 | 6528.400 | 84.16% | 0.382 | 0.437 | 1.144x |

## 판정 근거

- throughput aggregate 65.71%와 mean-latency aggregate 1.957x를 target 85% / max 2.0x와 비교해 `미달(65.7%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

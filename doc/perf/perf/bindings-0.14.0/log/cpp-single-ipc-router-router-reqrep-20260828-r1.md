# C++ paired measurement: Single / ipc / ROUTER_ROUTER_REQREP

- timestamp: 2026-08-28T07:33:19+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_073319_cpp0140-single-ipc-router-router-reqrep-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_073350_cpp0140-single-ipc-router-router-reqrep-r1.txt`
- status: 미달(65.8%)
- aggregate throughput ratio: 65.83% (target 85%)
- aggregate mean-latency ratio: 1.944x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 175399.600 | 105696.000 | 60.26% | 0.198 | 0.323 | 1.631x |
| 256 | 170504.200 | 98872.400 | 57.99% | 0.206 | 0.452 | 2.194x |
| 1024 | 162192.000 | 66648.200 | 41.09% | 0.215 | 0.884 | 4.112x |
| 65536 | 19240.400 | 14800.800 | 76.93% | 0.721 | 0.928 | 1.287x |
| 131072 | 13820.600 | 11326.400 | 81.95% | 0.431 | 0.512 | 1.188x |
| 262144 | 8810.000 | 6763.200 | 76.77% | 0.337 | 0.422 | 1.252x |

## 판정 근거

- throughput aggregate 65.83%와 mean-latency aggregate 1.944x를 target 85% / max 2.0x와 비교해 `미달(65.8%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

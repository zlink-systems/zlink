# C++ paired measurement: Single / ipc / DEALER_ROUTER

- timestamp: 2026-08-28T07:30:10+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_073010_cpp0140-single-ipc-dealer-router-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_073041_cpp0140-single-ipc-dealer-router-r1.txt`
- status: 미달(86.0%)
- aggregate throughput ratio: 86.01% (target 85%)
- aggregate mean-latency ratio: 9.751x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 920169.400 | 739045.200 | 80.32% | 24.809 | 33.075 | 1.333x |
| 256 | 777288.400 | 663474.000 | 85.36% | 0.216 | 10.362 | 47.972x |
| 1024 | 821174.400 | 625084.400 | 76.12% | 1.049 | 6.103 | 5.818x |
| 65536 | 39133.400 | 33383.800 | 85.31% | 0.230 | 0.293 | 1.274x |
| 131072 | 25986.200 | 24995.400 | 96.19% | 0.209 | 0.222 | 1.062x |
| 262144 | 17124.600 | 15884.200 | 92.76% | 0.185 | 0.194 | 1.049x |

## 판정 근거

- throughput aggregate 86.01%와 mean-latency aggregate 9.751x를 target 85% / max 2.0x와 비교해 `미달(86.0%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

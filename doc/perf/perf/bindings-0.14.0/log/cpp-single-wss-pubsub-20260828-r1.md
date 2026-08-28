# C++ paired measurement: Single / wss / PUBSUB

- timestamp: 2026-08-28T07:02:49+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_070249_cpp0140-single-wss-pubsub-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_070326_cpp0140-single-wss-pubsub-r1.txt`
- status: 미달(92.8%)
- aggregate throughput ratio: 92.78% (target 95%)
- aggregate mean-latency ratio: 1.138x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 749219.200 | 662897.400 | 88.48% | 84.126 | 90.484 | 1.076x |
| 256 | 614122.400 | 555776.000 | 90.50% | 54.462 | 63.095 | 1.159x |
| 1024 | 244856.200 | 239284.400 | 97.72% | 34.868 | 34.969 | 1.003x |
| 65536 | 10555.400 | 10351.600 | 98.07% | 12.163 | 12.382 | 1.018x |
| 131072 | 5988.000 | 5696.600 | 95.13% | 4.752 | 7.139 | 1.502x |
| 262144 | 2644.800 | 2294.800 | 86.77% | 0.668 | 0.717 | 1.073x |

## 판정 근거

- throughput aggregate 92.78%와 mean-latency aggregate 1.138x를 target 95% / max 2.0x와 비교해 `미달(92.8%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

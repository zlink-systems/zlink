# C++ paired measurement: Single / wss / PAIR

- timestamp: 2026-08-28T07:01:46+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_070145_cpp0140-single-wss-pair-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_070217_cpp0140-single-wss-pair-r1.txt`
- status: 미달(92.3%)
- aggregate throughput ratio: 92.34% (target 95%)
- aggregate mean-latency ratio: 1.094x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 951274.000 | 737077.200 | 77.48% | 94.674 | 110.118 | 1.163x |
| 256 | 780498.400 | 653001.200 | 83.66% | 35.597 | 40.368 | 1.134x |
| 1024 | 265655.800 | 263483.000 | 99.18% | 34.929 | 30.614 | 0.876x |
| 65536 | 10546.200 | 10024.000 | 95.05% | 10.410 | 13.010 | 1.250x |
| 131072 | 6255.000 | 6119.600 | 97.84% | 9.316 | 9.184 | 0.986x |
| 262144 | 3360.600 | 3388.400 | 100.83% | 7.373 | 8.504 | 1.153x |

## 판정 근거

- throughput aggregate 92.34%와 mean-latency aggregate 1.094x를 target 95% / max 2.0x와 비교해 `미달(92.3%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

# C++ paired measurement: Single / tls / PUBSUB

- timestamp: 2026-08-28T07:11:31+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_071131_cpp0140-single-tls-pubsub-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_071208_cpp0140-single-tls-pubsub-r1.txt`
- status: 미달(95.0%)
- aggregate throughput ratio: 94.98% (target 95%)
- aggregate mean-latency ratio: 1.010x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 727127.800 | 673570.600 | 92.63% | 0.111 | 0.181 | 1.631x |
| 256 | 746342.000 | 641791.800 | 85.99% | 1.553 | 0.473 | 0.305x |
| 1024 | 313712.200 | 344805.200 | 109.91% | 1.403 | 1.285 | 0.916x |
| 65536 | 12341.600 | 12497.800 | 101.27% | 0.716 | 0.645 | 0.901x |
| 131072 | 6167.800 | 5933.000 | 96.19% | 0.563 | 0.594 | 1.055x |
| 262144 | 2566.800 | 2153.800 | 83.91% | 0.520 | 0.651 | 1.252x |

## 판정 근거

- throughput aggregate 94.98%와 mean-latency aggregate 1.010x를 target 95% / max 2.0x와 비교해 `미달(95.0%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

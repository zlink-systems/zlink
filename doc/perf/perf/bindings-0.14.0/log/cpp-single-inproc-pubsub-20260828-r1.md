# C++ paired measurement: Single / inproc / PUBSUB

- timestamp: 2026-08-28T07:19:38+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_071938_cpp0140-single-inproc-pubsub-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_072016_cpp0140-single-inproc-pubsub-r1.txt`
- status: 통과(103.3%)
- aggregate throughput ratio: 103.33% (target 95%)
- aggregate mean-latency ratio: 1.097x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 749621.000 | 726296.400 | 96.89% | 0.041 | 0.048 | 1.171x |
| 256 | 666241.200 | 644818.200 | 96.78% | 0.045 | 0.040 | 0.889x |
| 1024 | 673206.800 | 633800.600 | 94.15% | 0.046 | 0.041 | 0.891x |
| 65536 | 82525.600 | 91653.400 | 111.06% | 0.014 | 0.023 | 1.643x |
| 131072 | 47753.800 | 47294.800 | 99.04% | 0.015 | 0.016 | 1.067x |
| 262144 | 14375.600 | 17551.000 | 122.09% | 0.026 | 0.024 | 0.923x |

## 판정 근거

- throughput aggregate 103.33%와 mean-latency aggregate 1.097x를 target 95% / max 2.0x와 비교해 `통과(103.3%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

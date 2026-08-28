# C++ paired measurement: Single / ipc / PUBSUB

- timestamp: 2026-08-28T07:27:52+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_072752_cpp0140-single-ipc-pubsub-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_072829_cpp0140-single-ipc-pubsub-r1.txt`
- status: 통과(103.5%)
- aggregate throughput ratio: 103.47% (target 95%)
- aggregate mean-latency ratio: 1.133x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 734252.400 | 652568.800 | 88.88% | 0.058 | 0.098 | 1.690x |
| 256 | 677667.600 | 585529.200 | 86.40% | 0.133 | 0.141 | 1.060x |
| 1024 | 694062.800 | 612247.200 | 88.21% | 0.466 | 0.111 | 0.238x |
| 65536 | 13167.400 | 13136.800 | 99.77% | 0.203 | 0.230 | 1.133x |
| 131072 | 6072.800 | 7473.000 | 123.06% | 0.194 | 0.273 | 1.407x |
| 262144 | 2554.600 | 3435.800 | 134.49% | 0.198 | 0.252 | 1.273x |

## 판정 근거

- throughput aggregate 103.47%와 mean-latency aggregate 1.133x를 target 95% / max 2.0x와 비교해 `통과(103.5%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

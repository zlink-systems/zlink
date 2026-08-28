# C++ paired measurement: Single / ipc / PAIR

- timestamp: 2026-08-28T07:26:49+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_072649_cpp0140-single-ipc-pair-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_072720_cpp0140-single-ipc-pair-r1.txt`
- status: 미달(87.2%)
- aggregate throughput ratio: 87.17% (target 95%)
- aggregate mean-latency ratio: 1.280x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 943810.800 | 739684.200 | 78.37% | 0.397 | 0.650 | 1.637x |
| 256 | 805563.400 | 674778.400 | 83.76% | 0.199 | 0.308 | 1.548x |
| 1024 | 868619.200 | 681398.600 | 78.45% | 0.287 | 0.338 | 1.178x |
| 65536 | 38851.800 | 37370.400 | 96.19% | 0.230 | 0.261 | 1.135x |
| 131072 | 28187.000 | 25425.800 | 90.20% | 0.194 | 0.220 | 1.134x |
| 262144 | 16983.200 | 16308.800 | 96.03% | 0.186 | 0.195 | 1.048x |

## 판정 근거

- throughput aggregate 87.17%와 mean-latency aggregate 1.280x를 target 95% / max 2.0x와 비교해 `미달(87.2%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

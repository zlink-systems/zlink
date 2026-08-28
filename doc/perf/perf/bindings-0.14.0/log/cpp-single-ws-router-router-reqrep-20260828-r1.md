# C++ paired measurement: Single / ws / ROUTER_ROUTER_REQREP

- timestamp: 2026-08-28T06:58:27+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_065827_cpp0140-single-ws-router-router-reqrep-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_065858_cpp0140-single-ws-router-router-reqrep-r1.txt`
- status: 미달(58.7%)
- aggregate throughput ratio: 58.73% (target 85%)
- aggregate mean-latency ratio: 2.573x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 167001.400 | 40251.400 | 24.10% | 0.263 | 1.554 | 5.909x | 167001.400/0.263 | 40251.400/1.554 |
| 256 | 100464.800 | 33998.400 | 33.84% | 0.670 | 1.850 | 2.761x | 100464.800/0.670 | 33998.400/1.850 |
| 1024 | 67164.600 | 24097.400 | 35.88% | 0.916 | 3.072 | 3.354x | 67164.600/0.916 | 24097.400/3.072 |
| 65536 | 10675.000 | 9090.200 | 85.15% | 1.121 | 1.302 | 1.161x | 10675.000/1.121 | 9090.200/1.302 |
| 131072 | 7471.000 | 6504.000 | 87.06% | 0.799 | 0.904 | 1.131x | 7471.000/0.799 | 6504.000/0.904 |
| 262144 | 5134.400 | 4433.400 | 86.35% | 0.582 | 0.654 | 1.124x | 5134.400/0.582 | 4433.400/0.654 |

## 판정 근거

- throughput aggregate 58.73%와 mean-latency aggregate 2.573x를 target 85% / max 2.0x와 비교해 `미달(58.7%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

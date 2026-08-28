# C++ paired measurement: Single / tls / ROUTER_ROUTER_REQREP

- timestamp: 2026-08-28T07:16:58+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_071658_cpp0140-single-tls-router-router-reqrep-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_071729_cpp0140-single-tls-router-router-reqrep-r1.txt`
- status: 미달(70.1%)
- aggregate throughput ratio: 70.08% (target 85%)
- aggregate mean-latency ratio: 1.820x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 171757.800 | 83762.800 | 48.77% | 0.236 | 0.695 | 2.945x |
| 256 | 156928.000 | 79717.400 | 50.80% | 0.282 | 0.735 | 2.606x |
| 1024 | 93260.600 | 42269.000 | 45.32% | 0.644 | 1.481 | 2.300x |
| 65536 | 5841.200 | 5245.000 | 89.79% | 2.052 | 2.270 | 1.106x |
| 131072 | 3500.600 | 3224.400 | 92.11% | 1.711 | 1.838 | 1.074x |
| 262144 | 1882.000 | 1763.200 | 93.69% | 1.868 | 1.655 | 0.886x |

## 판정 근거

- throughput aggregate 70.08%와 mean-latency aggregate 1.820x를 target 85% / max 2.0x와 비교해 `미달(70.1%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

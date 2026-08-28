# C++ paired measurement: Single / tls / DEALER_ROUTER_REQREP

- timestamp: 2026-08-28T07:14:52+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_071452_cpp0140-single-tls-dealer-router-reqrep-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_071523_cpp0140-single-tls-dealer-router-reqrep-r1.txt`
- status: 미달(70.4%)
- aggregate throughput ratio: 70.45% (target 85%)
- aggregate mean-latency ratio: 1.783x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 179854.600 | 86923.800 | 48.33% | 0.245 | 0.669 | 2.731x |
| 256 | 167537.600 | 80792.400 | 48.22% | 0.279 | 0.727 | 2.606x |
| 1024 | 92166.800 | 44149.200 | 47.90% | 0.663 | 1.414 | 2.133x |
| 65536 | 5727.800 | 5293.000 | 92.41% | 2.092 | 2.249 | 1.075x |
| 131072 | 3502.400 | 3155.000 | 90.08% | 1.936 | 2.200 | 1.136x |
| 262144 | 1888.400 | 1808.200 | 95.75% | 1.585 | 1.614 | 1.018x |

## 판정 근거

- throughput aggregate 70.45%와 mean-latency aggregate 1.783x를 target 85% / max 2.0x와 비교해 `미달(70.4%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

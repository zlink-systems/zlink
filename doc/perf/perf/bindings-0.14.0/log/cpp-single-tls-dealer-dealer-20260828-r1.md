# C++ paired measurement: Single / tls / DEALER_DEALER

- timestamp: 2026-08-28T07:12:46+09:00
- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_071245_cpp0140-single-tls-dealer-dealer-r1.txt`
- C++ report: `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260828_071317_cpp0140-single-tls-dealer-dealer-r1.txt`
- status: 미달(91.9%)
- aggregate throughput ratio: 91.91% (target 95%)
- aggregate mean-latency ratio: 0.866x (max 2.0x)
- `runs=1`; terrain-reading only, not a final five-run-median determination.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 64 | 804761.200 | 668417.200 | 83.06% | 146.646 | 155.662 | 1.061x |
| 256 | 840392.000 | 713098.400 | 84.85% | 46.605 | 0.548 | 0.012x |
| 1024 | 397051.000 | 413792.000 | 104.22% | 1.355 | 1.147 | 0.846x |
| 65536 | 12794.000 | 12179.600 | 95.20% | 0.719 | 0.775 | 1.078x |
| 131072 | 7336.800 | 7055.200 | 96.16% | 0.696 | 0.735 | 1.056x |
| 262144 | 4128.000 | 3631.200 | 87.97% | 0.754 | 0.862 | 1.143x |

## 판정 근거

- throughput aggregate 91.91%와 mean-latency aggregate 0.866x를 target 95% / max 2.0x와 비교해 `미달(91.9%)`이다.
- 개선 후보(미구현): C++ public hot path의 dispatch, callback, allocation/copy와 latency sampling을 C reference와 profile 대조한다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

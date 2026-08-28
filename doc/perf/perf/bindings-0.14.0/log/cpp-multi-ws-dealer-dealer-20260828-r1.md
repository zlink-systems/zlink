# C++ paired measurement: Multi / ws / MULTI_DEALER_DEALER

- timestamp: 2026-08-28T08:04:42+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_080442_cpp0140-multi-ws-dealer-dealer-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_080518_cpp0140-multi-ws-dealer-dealer-r1.txt`
- status: 미달(52.1%)
- aggregate throughput ratio: 52.06% (target 95%)
- aggregate mean-latency ratio median: 1.204x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 695499.000 | 323780.000 | 46.55% | 789.706 | 7523.673 | 9.527x | 695499.000/789.706 | 323780.000/7523.673 |
| 256 | 897085.800 | 265700.000 | 29.62% | 1.746 | 2379.792 | 1362.997x | 897085.800/1.746 | 265700.000/2379.792 |
| 1024 | 650649.400 | 290920.000 | 44.71% | 669.851 | 1.020 | 0.002x | 650649.400/669.851 | 290920.000/1.020 |
| 4096 | 348518.200 | 219060.000 | 62.85% | 617.216 | 928.324 | 1.504x | 348518.200/617.216 | 219060.000/928.324 |
| 65536 | 69002.200 | 41866.800 | 60.67% | 166.109 | 103.606 | 0.624x | 69002.200/166.109 | 41866.800/103.606 |
| 131072 | 38137.800 | 25906.200 | 67.93% | 138.401 | 125.045 | 0.903x | 38137.800/138.401 | 25906.200/125.045 |

## 판정 근거
- throughput aggregate 52.06%와 latency 중앙값 1.204x로 판정했다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.
- 직전 C report의 all-size FAIL/status partial은 같은 manifest에서 재현되지 않았다. 원인은 server exit 1 외 추가 증거가 없어 C runner 결함으로 단정하지 않는다.
- 개선 후보(미구현): C++ multi DEALER_DEALER public send/receive dispatch와 C reference의 hot-path 비용을 profile로 대조한다.

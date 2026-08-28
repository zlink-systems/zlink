# C++ paired measurement: Multi / tcp / MULTI_DEALER_DEALER

- timestamp: 2026-08-28T07:49:31+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_074931_cpp0140-multi-tcp-dealer-dealer-r1-recheck.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_075015_cpp0140-multi-tcp-dealer-dealer-r1-recheck.txt`
- status: 미달(54.5%)
- aggregate throughput ratio: 54.54% (target 95%)
- aggregate mean-latency ratio median: 0.442x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 1024194.800 | 397740.000 | 38.83% | 57.871 | 0.721 | 0.012x | 1024194.800/57.871 | 397740.000/0.721 |
| 256 | 968182.200 | 334200.000 | 34.52% | 1.391 | 0.448 | 0.322x | 968182.200/1.391 | 334200.000/0.448 |
| 1024 | 860312.800 | 331980.000 | 38.59% | 25.210 | 0.404 | 0.016x | 860312.800/25.210 | 331980.000/0.404 |
| 4096 | 357887.800 | 296680.000 | 82.90% | 662.826 | 472.060 | 0.712x | 357887.800/662.826 | 296680.000/472.060 |
| 65536 | 113277.400 | 63804.600 | 56.33% | 66.103 | 135.770 | 2.054x | 113277.400/66.103 | 63804.600/135.770 |
| 131072 | 54641.800 | 41583.600 | 76.10% | 105.303 | 59.159 | 0.562x | 54641.800/105.303 | 41583.600/59.159 |

## 판정 근거
- throughput aggregate 54.54%와 latency 중앙값 0.442x로 판정했다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.
- 직전 C report의 all-size FAIL/status partial은 같은 manifest에서 재현되지 않았다. 원인은 server exit 1 외 추가 증거가 없어 C runner 결함으로 단정하지 않는다.
- 개선 후보(미구현): C++ multi DEALER_DEALER public send/receive dispatch와 C reference의 hot-path 비용을 profile로 대조한다.

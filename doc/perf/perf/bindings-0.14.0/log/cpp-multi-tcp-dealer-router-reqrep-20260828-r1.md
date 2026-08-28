# C++ paired measurement: Multi / tcp / MULTI_DEALER_ROUTER_REQREP

- timestamp: 2026-08-28T07:53:31+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_075331_cpp0140-multi-tcp-MULTI_DEALER_ROUTER_REQREP-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_075405_cpp0140-multi-tcp-MULTI_DEALER_ROUTER_REQREP-r1.txt`
- status: 통과(97.6%)
- aggregate throughput ratio: 97.62% (target 85%)
- aggregate mean-latency ratio median: 1.415x (max 2.0x)
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072.
- runtime: `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.14.0`
- META: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded in both reports.

| Size | C throughput | C++ throughput | Ratio | C mean latency | C++ mean latency | Latency ratio | C raw (throughput/latency) | C++ raw (throughput/latency) |
|---:|---:|---:|---:|---:|---:|---:|---|---|
| 64 | 79159.600 | 72176.800 | 91.18% | 0.407 | 0.572 | 1.405x | 79159.600/0.407 | 72176.800/0.572 |
| 256 | 61987.600 | 56286.600 | 90.80% | 0.514 | 0.694 | 1.350x | 61987.600/0.514 | 56286.600/0.694 |
| 1024 | 65324.000 | 60063.800 | 91.95% | 0.463 | 0.709 | 1.531x | 65324.000/0.463 | 60063.800/0.709 |
| 4096 | 60114.200 | 57622.000 | 95.85% | 0.516 | 0.735 | 1.424x | 60114.200/0.516 | 57622.000/0.735 |
| 65536 | 20950.600 | 21935.000 | 104.70% | 1.430 | 2.043 | 1.429x | 20950.600/1.430 | 21935.000/2.043 |
| 131072 | 13696.200 | 15238.400 | 111.26% | 2.306 | 2.929 | 1.270x | 13696.200/2.306 | 15238.400/2.929 |

## 판정 근거
- throughput aggregate 97.62%와 latency 중앙값 1.415x로 판정했다.
- 이번 run은 측정 전용이며 binding source와 runner를 수정하지 않았다.

# C++ paired measurement: Multi / tls / MULTI_PUBSUB

- timestamp: 2026-08-28T08:33:23+09:00
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260828_083323_cpp0140-multi-tls-pubsub-r1.txt`
- C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260828_083357_cpp0140-multi-tls-pubsub-r1.txt`
- status: 통과(156.2%)
- aggregate throughput ratio: 156.21% (target 95%); aggregate mean-latency ratio median: 0.831x (max 2.0x).
- `runs=1`; this is a terrain-reading result, not a final five-run-median determination.
- inventory: clients=100; server/client I/O threads=4/4; sizes=64,256,1024,4096,65536,131072; duration=5s; part-count=2.
- META in both reports: commit=510f161e8f, core_source=local, core_version=0.14.0, core_runtime recorded; native SHA matched before measurement.

| Size | C raw (throughput / mean latency ms) | C++ raw (throughput / mean latency ms) | Throughput / latency ratio |
|---:|---:|---:|---:|
| 64 | 317182.600 / 1741.293 | 637288.400 / 1770.499 | 200.92% / 1.017x |
| 256 | 325502.200 / 1633.095 | 719746.600 / 1566.707 | 221.12% / 0.959x |
| 1024 | 461540.600 / 1329.357 | 782810.200 / 672.930 | 169.61% / 0.506x |
| 4096 | 292758.000 / 416.839 | 318240.600 / 343.189 | 108.70% / 0.823x |
| 65536 | 27912.400 / 240.101 | 33081.000 / 184.198 | 118.52% / 0.767x |
| 131072 | 14582.600 / 209.509 | 17261.400 / 175.521 | 118.37% / 0.838x |

## 판정 근거

- throughput aggregate와 latency 중앙값이 모두 목표를 만족했다.

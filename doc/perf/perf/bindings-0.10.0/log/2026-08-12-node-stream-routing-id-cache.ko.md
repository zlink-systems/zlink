# Node STREAM routing-id cache 결과

`MULTI_STREAM / tcp`, Core `0.10.1` release runtime, clients 100, duration 1초, runs 1,
balanced auto-HWM에서 C를 먼저 실행하고 Node를 다음에 실행했다. 병렬 실행은 없었다.

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 353,260 ops/s | 56,132 ops/s | 15.890% |
| 256B | 319,057 ops/s | 57,016 ops/s | 17.870% |
| 1024B | 333,583 ops/s | 56,377 ops/s | 16.900% |
| 65536B | 66,048 ops/s | 41,076 ops/s | 62.191% |

throughput ratio 산술평균은 28.213%다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_100538_node-stream-rid-cache-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_100613_node-stream-rid-cache.txt`

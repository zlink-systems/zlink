# Node STREAM flat callback 결과

`MULTI_STREAM / tcp`, Core `0.10.1` release runtime, clients 100, duration 1초, runs 1,
balanced auto-HWM에서 C를 먼저 실행하고 Node를 다음에 실행했다. 병렬 실행은 없었다.

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 359,336 ops/s | 56,044 ops/s | 15.597% |
| 256B | 356,578 ops/s | 58,620 ops/s | 16.440% |
| 1024B | 345,084 ops/s | 57,444 ops/s | 16.646% |
| 65536B | 60,659 ops/s | 42,266 ops/s | 69.678% |

throughput ratio 산술평균은 29.590%다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_101338_node-stream-flat-callback-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_101415_node-stream-flat-callback.txt`

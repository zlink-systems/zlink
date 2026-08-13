# Node STREAM WSS 결과

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 221,074.5 | 133,253.5 | 60.28% |
| 256B | 210,962.5 | 132,565.5 | 62.84% |
| 1024B | 196,782.5 | 122,633.0 | 62.32% |
| 65536B | 11,709.0 | 11,507.5 | 98.28% |
| 산술평균 | - | - | 70.93% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Node 22.23.2
- 대상: `MULTI_STREAM / wss`, balanced auto-HWM
- C report: `/tmp/zlink-node-stream-wss-c/multi/report/perf_c_multi_linux_20260813_224719_node-stream-wss-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260813_224752.txt`

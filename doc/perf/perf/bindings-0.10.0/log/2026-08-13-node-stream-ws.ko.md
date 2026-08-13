# Node STREAM WS 결과

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 315,867.0 | 166,586.5 | 52.74% |
| 256B | 295,290.0 | 155,389.5 | 52.62% |
| 1024B | 261,816.5 | 153,291.5 | 58.55% |
| 65536B | 18,705.0 | 19,071.0 | 101.96% |
| 산술평균 | - | - | 66.47% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Node 22.23.2
- 대상: `MULTI_STREAM / ws`, balanced auto-HWM
- C report: `/tmp/zlink-node-stream-ws-c/multi/report/perf_c_multi_linux_20260813_224602_node-stream-ws-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260813_224628.txt`

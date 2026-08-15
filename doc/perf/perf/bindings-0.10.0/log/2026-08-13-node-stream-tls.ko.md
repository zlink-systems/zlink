# Node STREAM TLS 결과

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 239,947.0 | 135,561.5 | 56.50% |
| 256B | 231,434.5 | 138,504.5 | 59.85% |
| 1024B | 209,253.5 | 131,668.0 | 62.92% |
| 65536B | 18,772.0 | 19,324.0 | 102.94% |
| 산술평균 | - | - | 70.55% |

- Core: release `0.10.1`
- 대상: `MULTI_STREAM / tls`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 Node 22.23.2 실행, 병렬 실행 없음
- C report: `/tmp/zlink-node-stream-tls-c/multi/report/perf_c_multi_linux_20260813_224447_node-stream-tls-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260813_224515.txt`

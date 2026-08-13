# Node PUB/SUB TLS 결과

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 1,237,255.5 | 415,798.5 | 33.61% |
| 256B | 1,611,292.5 | 517,230.0 | 32.10% |
| 1024B | 1,137,275.5 | 396,417.5 | 34.86% |
| 4096B | 434,965.5 | 219,379.5 | 50.44% |
| 65536B | 39,771.5 | 28,961.0 | 72.82% |
| 131072B | 19,737.0 | 15,027.5 | 76.14% |
| 산술평균 | - | - | 49.99% |

- Core: release `0.10.1`
- 대상: `MULTI_PUBSUB / tls`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 Node 22.23.2 실행, 병렬 실행 없음
- C report: `/tmp/zlink-node-ps-tls-c/multi/report/perf_c_multi_linux_20260813_223840_node-ps-tls-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260813_223933.txt`

# Node DEALER/ROUTER send/send WSS 결과

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 139,646.0 | 90,117.5 | 64.53% |
| 256B | 142,034.0 | 89,516.0 | 63.02% |
| 1024B | 132,547.5 | 85,729.0 | 64.68% |
| 4096B | 98,631.0 | 67,926.0 | 68.87% |
| 65536B | 13,028.0 | 12,698.0 | 97.47% |
| 131072B | 7,573.5 | 7,534.5 | 99.49% |
| 산술평균 | - | - | 76.34% |

- Core: release `0.10.1`
- 대상: `MULTI_DEALER_ROUTER_SENDSEND / wss`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 Node 22.23.2 실행, 병렬 실행 없음
- C report: `/tmp/zlink-node-dr-wss-c/multi/report/perf_c_multi_linux_20260813_221911_node-dr-wss-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260813_222030.txt`

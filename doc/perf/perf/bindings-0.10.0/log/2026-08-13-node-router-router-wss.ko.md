# Node ROUTER/ROUTER send/send WSS 결과

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 141,656.0 | 91,640.0 | 64.69% |
| 256B | 146,826.0 | 68,116.0 | 46.39% |
| 1024B | 97,968.5 | 75,060.0 | 76.62% |
| 4096B | 104,291.5 | 62,103.5 | 59.55% |
| 65536B | 12,899.0 | 13,396.5 | 103.86% |
| 131072B | 7,985.0 | 7,521.5 | 94.20% |
| 산술평균 | - | - | 74.22% |

- Core: release `0.10.1`
- 대상: `MULTI_ROUTER_ROUTER_SENDSEND / wss`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 Node 22.23.2 실행, 병렬 실행 없음
- C parity: Node client은 C와 같이 poll ready socket만 receive하고 active deadline을 넘지 않게 대기한다.
- C report: `/tmp/zlink-node-rr-wss-parity-c/multi/report/perf_c_multi_linux_20260813_223414_node-rr-wss-parity-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260813_223533.txt`

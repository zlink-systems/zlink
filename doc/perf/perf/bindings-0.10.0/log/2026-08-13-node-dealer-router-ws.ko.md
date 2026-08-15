# Node DEALER/ROUTER send/send WS 결과

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 178,832.5 | 116,521.0 | 65.16% |
| 256B | 148,749.0 | 103,626.5 | 69.67% |
| 1024B | 170,396.5 | 103,812.0 | 60.92% |
| 4096B | 143,562.5 | 86,534.0 | 60.28% |
| 65536B | 33,277.5 | 25,824.0 | 77.60% |
| 131072B | 17,481.0 | 14,005.5 | 80.12% |
| 산술평균 | - | - | 68.96% |

- Core: release `0.10.1`
- 대상: `MULTI_DEALER_ROUTER_SENDSEND / ws`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 Node 22.23.2 실행, 병렬 실행 없음
- C parity: Node client은 C와 같이 poll ready socket만 receive하고 active deadline을 넘지 않게 대기한다. 이 변경으로 전수 `recv(DONT_WAIT)` probe를 제거했다.
- C report: `/tmp/zlink-node-dr-ws-parity-c/multi/report/perf_c_multi_linux_20260813_223121_node-dr-ws-parity-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260813_223233.txt`

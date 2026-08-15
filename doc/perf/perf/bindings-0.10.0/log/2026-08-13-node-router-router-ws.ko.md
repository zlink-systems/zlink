# Node ROUTER/ROUTER send/send WS 결과

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 195,421.0 | 100,463.5 | 51.41% |
| 256B | 186,532.5 | 89,689.0 | 48.08% |
| 1024B | 184,043.5 | 88,973.0 | 48.34% |
| 4096B | 150,802.5 | 79,897.0 | 52.98% |
| 65536B | 35,078.5 | 23,698.0 | 67.56% |
| 131072B | 18,216.5 | 13,539.0 | 74.32% |
| 산술평균 | - | - | 57.12% |

- Core: release `0.10.1`
- 대상: `MULTI_ROUTER_ROUTER_SENDSEND / ws`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 Node 22.23.2 실행, 병렬 실행 없음
- C parity: Node client은 C와 같이 poll ready socket만 receive하고 active deadline을 넘지 않게 대기한다. 이 변경으로 전수 `recv(DONT_WAIT)` probe를 제거했다.
- C report: `/tmp/zlink-node-rr-ws-parity-c/multi/report/perf_c_multi_linux_20260813_222845_node-rr-ws-parity-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260813_223001.txt`

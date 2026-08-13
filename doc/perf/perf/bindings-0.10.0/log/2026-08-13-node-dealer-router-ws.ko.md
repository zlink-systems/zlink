# Node DEALER/ROUTER send/send WS 결과

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 190,191.0 | 116,116.0 | 61.05% |
| 256B | 184,356.0 | 108,432.5 | 58.82% |
| 1024B | 177,746.5 | 85,859.5 | 48.30% |
| 4096B | 150,787.5 | 89,249.0 | 59.19% |
| 65536B | 34,741.0 | 27,693.5 | 79.71% |
| 131072B | 18,411.0 | 14,620.0 | 79.41% |
| 산술평균 | - | - | 64.41% |

- Core: release `0.10.1`
- 대상: `MULTI_DEALER_ROUTER_SENDSEND / ws`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 Node 22.23.2 실행, 병렬 실행 없음
- C report: `/tmp/zlink-node-dr-ws-c/multi/report/perf_c_multi_linux_20260813_221730_node-dr-ws-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260813_221842.txt`

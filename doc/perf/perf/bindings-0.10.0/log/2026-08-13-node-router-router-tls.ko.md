# Node ROUTER/ROUTER send/send TLS 결과

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 167,092.0 | 95,092.0 | 56.91% |
| 256B | 160,376.5 | 86,473.0 | 53.92% |
| 1024B | 147,648.0 | 83,716.5 | 56.70% |
| 4096B | 123,329.0 | 71,053.5 | 57.61% |
| 65536B | 17,821.0 | 16,080.5 | 90.23% |
| 131072B | 9,781.0 | 9,225.5 | 94.32% |
| 산술평균 | - | - | 68.28% |

- Core: release `0.10.1`
- 대상: `MULTI_ROUTER_ROUTER_SENDSEND / tls`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 Node 22.23.2 실행, 병렬 실행 없음
- C parity: Node client은 C와 같이 poll ready socket만 receive하고 active deadline을 넘지 않게 대기한다.
- C report: `/tmp/zlink-node-rr-tls-parity-c/multi/report/perf_c_multi_linux_20260813_223608_node-rr-tls-parity-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260813_223735.txt`

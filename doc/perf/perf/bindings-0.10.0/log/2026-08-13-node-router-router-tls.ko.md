# Node ROUTER/ROUTER send/send TLS 결과

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 163,054.5 | 87,552.5 | 53.70% |
| 256B | 162,250.5 | 80,467.5 | 49.59% |
| 1024B | 154,084.0 | 80,406.5 | 52.18% |
| 4096B | 121,172.0 | 68,808.5 | 56.79% |
| 65536B | 16,440.5 | 15,295.0 | 93.03% |
| 131072B | 9,878.5 | 8,964.5 | 90.75% |
| 산술평균 | - | - | 66.01% |

- Core: release `0.10.1`
- 대상: `MULTI_ROUTER_ROUTER_SENDSEND / tls`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 Node 22.23.2 실행, 병렬 실행 없음
- C report: `/tmp/zlink-node-rr-tls-c/multi/report/perf_c_multi_linux_20260813_222117_node-rr-tls-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260813_222235.txt`

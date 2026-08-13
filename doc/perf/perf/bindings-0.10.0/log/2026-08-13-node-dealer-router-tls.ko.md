# Node DEALER/ROUTER send/send TLS 결과

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 171,079.0 | 103,096.5 | 60.26% |
| 256B | 167,007.5 | 103,299.0 | 61.85% |
| 1024B | 155,014.5 | 99,528.5 | 64.21% |
| 4096B | 128,371.5 | 74,523.5 | 58.05% |
| 65536B | 18,098.0 | 16,008.0 | 88.45% |
| 131072B | 9,722.5 | 9,087.5 | 93.47% |
| 산술평균 | - | - | 71.05% |

- Core: release `0.10.1`
- 대상: `MULTI_DEALER_ROUTER_SENDSEND / tls`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 Node 22.23.2 실행, 병렬 실행 없음
- C report: `/tmp/zlink-node-dr-tls-c/multi/report/perf_c_multi_linux_20260813_221541_node-dr-tls-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260813_221700.txt`

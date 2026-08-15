# Node DEALER/DEALER TLS 결과

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 3,026,398.5 | 364,131.0 | 12.03% |
| 256B | 1,558,747.5 | 784,122.5 | 50.31% |
| 1024B | 966,543.0 | 461,437.0 | 47.74% |
| 4096B | 401,653.0 | 188,541.5 | 46.94% |
| 65536B | 34,758.5 | 37,659.0 | 108.35% |
| 131072B | 19,050.0 | 20,381.0 | 106.98% |
| 산술평균 | - | - | 62.06% |

- Core: release `0.10.1`
- 대상: `MULTI_DEALER_DEALER / tls`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 Node 22.23.2 실행, 병렬 실행 없음
- C report: `/tmp/zlink-node-dd-tls-c/multi/report/perf_c_multi_linux_20260813_220647_node-dd-tls-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260813_220813.txt`

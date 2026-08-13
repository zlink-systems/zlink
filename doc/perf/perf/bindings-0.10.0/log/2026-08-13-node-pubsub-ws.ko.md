# Node PUB/SUB WS 결과

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 1,240,114.0 | 420,125.0 | 33.88% |
| 256B | 1,507,575.0 | 534,727.5 | 35.47% |
| 1024B | 1,272,103.5 | 398,011.0 | 31.29% |
| 4096B | 517,849.0 | 235,023.5 | 45.38% |
| 65536B | 66,233.5 | 53,882.5 | 81.35% |
| 131072B | 35,354.0 | 27,660.5 | 78.24% |
| 산술평균 | - | - | 50.94% |

- Core: release `0.10.1`
- 대상: `MULTI_PUBSUB / ws`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 Node 22.23.2 실행, 병렬 실행 없음
- C report: `/tmp/zlink-node-ps-ws-c/multi/report/perf_c_multi_linux_20260813_224106_node-ps-ws-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260813_224200.txt`

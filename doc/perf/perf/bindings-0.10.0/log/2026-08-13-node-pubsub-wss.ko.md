# Node PUB/SUB WSS 결과

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 1,342,572.5 | 418,324.5 | 31.16% |
| 256B | 1,485,805.0 | 512,552.0 | 34.50% |
| 1024B | 878,532.0 | 354,003.0 | 40.29% |
| 4096B | 294,706.5 | 157,481.0 | 53.44% |
| 65536B | 27,428.0 | 21,470.5 | 78.28% |
| 131072B | 14,978.5 | 11,675.0 | 77.95% |
| 산술평균 | - | - | 52.60% |

- Core: release `0.10.1`
- 대상: `MULTI_PUBSUB / wss`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 Node 22.23.2 실행, 병렬 실행 없음
- C report: `/tmp/zlink-node-ps-wss-c/multi/report/perf_c_multi_linux_20260813_224258_node-ps-wss-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260813_224351.txt`

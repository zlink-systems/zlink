# Node DEALER/DEALER WSS 결과

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 3,093,785.0 | 537,385.0 | 17.37% |
| 256B | 1,409,859.0 | 706,164.5 | 50.09% |
| 1024B | 775,296.5 | 369,752.0 | 47.69% |
| 4096B | 248,845.0 | 171,299.5 | 68.84% |
| 65536B | 26,699.0 | 23,811.5 | 89.18% |
| 131072B | 12,345.0 | 13,862.5 | 112.29% |
| 산술평균 | - | - | 64.24% |

- Core: release `0.10.1`
- 대상: `MULTI_DEALER_DEALER / wss`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 Node 22.23.2 실행, 병렬 실행 없음
- C report: `/tmp/zlink-node-dd-wss-c/multi/report/perf_c_multi_linux_20260813_221330_node-dd-wss-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260813_221448.txt`

# Node DEALER/DEALER WS 결과

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 2,769,173.5 | 509,291.0 | 18.39% |
| 256B | 1,575,820.5 | 709,196.5 | 45.00% |
| 1024B | 1,013,383.5 | 479,851.0 | 47.35% |
| 4096B | 497,996.5 | 222,894.0 | 44.76% |
| 65536B | 71,963.5 | 51,021.0 | 70.90% |
| 131072B | 30,919.0 | 27,702.0 | 89.60% |
| 산술평균 | - | - | 52.67% |

- Core: release `0.10.1`
- 대상: `MULTI_DEALER_DEALER / ws`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: 각 비교에서 C 종료 후 Node 22.23.2 실행, 병렬 실행 없음
- 64KiB: 전체 실행의 Node client가 `Connection refused`로 끝나서, 같은 조건의 단일 C/Node 비교 결과를 사용했다. 단일 비교는 두 runner 모두 `status=complete`로 끝났다.
- C report: `/tmp/zlink-node-dd-ws-c/multi/report/perf_c_multi_linux_20260813_220917_node-dd-ws-c.txt`, 64KiB `/tmp/zlink-node-dd-ws-64-c/multi/report/perf_c_multi_linux_20260813_221048_node-dd-ws-64-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260813_221025.txt`, 64KiB `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260813_221220.txt`

# Node TCP PUBSUB 최종 측정 결과

## 조건

- 대상: `MULTI_PUBSUB / tcp`
- Core: release `0.10.1`
- 순서: C를 단독 실행한 뒤 Node를 단독 실행
- clients: `100`
- duration: `1초`
- runs: `1`
- HWM: balanced auto-HWM

## 결과

| Size | C Kmsg/s | Node Kmsg/s | Node/C |
|---|---:|---:|---:|
| 64B | 1512.172 | 426.902 | 28.23% |
| 256B | 1453.354 | 445.748 | 30.67% |
| 1024B | 1343.460 | 390.529 | 29.07% |
| 4096B | 571.253 | 188.619 | 33.02% |
| 65536B | 111.687 | 51.551 | 46.16% |
| 131072B | 56.750 | 29.047 | 51.18% |
| 산술 평균 | - | - | **36.39%** |

산술평균 36.39%는 Node simple one-way 최소 기준 35%를 충족한다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_140358_node-pubsub-tcp-current-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_140443_node-pubsub-tcp-current-node.txt`

# Node PUBSUB 빈 topic 생략 후보 결과

## 측정 조건

- Core: `0.10.1` release runtime
- 순서: C를 먼저 실행하고 Node를 다음에 실행
- 병렬 실행: 없음
- 대상: `MULTI_PUBSUB / tcp`
- size: 64·256·1024·4096·65536·131072B
- clients: 100, duration: 1초, runs: 1, balanced auto-HWM

## 측정값

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 1,681,640 msg/s | 288,687 msg/s | 17.167% |
| 256B | 1,665,837 msg/s | 320,063 msg/s | 19.213% |
| 1024B | 1,545,663 msg/s | 271,624 msg/s | 17.573% |
| 4096B | 606,086 msg/s | 163,881 msg/s | 27.039% |
| 65536B | 101,666 msg/s | 42,606 msg/s | 41.908% |
| 131072B | 52,740 msg/s | 25,021 msg/s | 47.444% |
| 평균 | - | - | 28.391% |

빈 topic을 native raw envelope에서 생략하고 TypeScript materialization에서 빈 문자열로 복원했다.
현재 채택값 30.62%보다 낮아 원복했다. native-to-JavaScript 문자열 생성 감소보다 raw object의
property shape 변화와 fallback 처리 비용이 더 컸다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_105003_node-pub-empty-topic-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_105036_node-pub-empty-topic.txt`

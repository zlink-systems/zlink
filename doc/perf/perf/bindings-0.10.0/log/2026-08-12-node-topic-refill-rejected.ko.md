# Node TopicMessage 단일 part 재충전 후보 결과

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
| 64B | 1,686,378 msg/s | 314,744 msg/s | 18.664% |
| 256B | 1,596,695 msg/s | 334,486 msg/s | 20.948% |
| 1024B | 1,191,118 msg/s | 293,104 msg/s | 24.608% |
| 4096B | 545,189 msg/s | 150,812 msg/s | 27.662% |
| 65536B | 109,179 msg/s | 42,381 msg/s | 38.819% |
| 131072B | 59,261 msg/s | 26,380 msg/s | 44.517% |

throughput ratio 산술평균은 29.203%다. caller-provided `TopicMessage`의 단일 part에서
기존 `Message` facade와 frozen parts 배열을 재사용하는 방식은 현재 채택값 30.620%보다 낮았다.
따라서 변경은 원복했다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_102555_node-topic-refill-standard-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_102628_node-topic-refill-standard.txt`

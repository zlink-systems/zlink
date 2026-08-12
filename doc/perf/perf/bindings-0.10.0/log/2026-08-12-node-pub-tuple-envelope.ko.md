# Node PUBSUB single-part tuple envelope 결과

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
| 64B | 1,521,282 msg/s | 330,999 msg/s | 21.758% |
| 256B | 1,614,750 msg/s | 334,570 msg/s | 20.719% |
| 1024B | 1,444,087 msg/s | 289,982 msg/s | 20.081% |
| 4096B | 468,358 msg/s | 166,944 msg/s | 35.646% |
| 65536B | 102,720 msg/s | 43,677 msg/s | 42.520% |
| 131072B | 48,979 msg/s | 23,125 msg/s | 47.214% |
| 평균 | - | - | 31.323% |

routing ID가 없는 single-part SUB receive에서 native raw `{ data, topic }` object 대신 `[data, topic]`
tuple을 사용했다. routed 또는 multipart receive는 기존 object envelope를 유지한다. 이전 채택값
31.05%보다 0.28%p 높아 채택했다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_110629_node-pub-tuple-envelope-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_110706_node-pub-tuple-envelope.txt`

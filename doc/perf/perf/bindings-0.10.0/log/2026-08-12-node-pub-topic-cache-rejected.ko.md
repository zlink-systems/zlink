# Node PUBSUB 최근 topic cache 후보 결과

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
| 64B | 1,686,378 msg/s | 299,682 msg/s | 17.771% |
| 256B | 1,596,695 msg/s | 311,881 msg/s | 19.533% |
| 1024B | 1,191,118 msg/s | 268,103 msg/s | 22.509% |
| 4096B | 545,189 msg/s | 135,323 msg/s | 24.821% |
| 65536B | 109,179 msg/s | 43,708 msg/s | 40.033% |
| 131072B | 59,261 msg/s | 26,037 msg/s | 43.936% |

throughput ratio 산술평균은 28.100%다. socket별 최근 topic JS string cache는 `napi_create_string_utf8`
호출을 줄였지만, cache lookup과 reference 관리 비용 때문에 현재 채택값 30.620%보다 낮았다.
변경은 원복했다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_102555_node-topic-refill-standard-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_103143_node-pub-topic-cache.txt`

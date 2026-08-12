# Node PUBSUB native view 후보 결과

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
| 64B | 1,729,769 msg/s | 152,206 msg/s | 8.799% |
| 256B | 1,681,882 msg/s | 163,121 msg/s | 9.699% |

일반 SUB 수신도 native frame과 external Buffer를 사용하도록 바꿨다. 64B·256B 결과가 현재
managed Buffer 경로보다 현저히 낮았고 이후 runner도 complete report를 만들지 못했다. native frame
handle 생성·refcount 비용이 payload copy 제거 이득보다 커서 즉시 원복했다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_104420_node-pub-native-view-c.txt`
- Node partial report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_104440_node-pub-native-view.txt`

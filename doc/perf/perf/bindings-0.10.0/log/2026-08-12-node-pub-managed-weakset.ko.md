# Node PUBSUB managed Message writable-state 분리 결과

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
| 64B | 1,446,986 msg/s | 295,885 msg/s | 20.448% |
| 256B | 1,601,068 msg/s | 336,216 msg/s | 20.999% |
| 1024B | 1,294,391 msg/s | 288,663 msg/s | 22.301% |
| 4096B | 506,464 msg/s | 175,138 msg/s | 34.581% |
| 65536B | 92,657 msg/s | 38,004 msg/s | 41.015% |
| 131072B | 53,751 msg/s | 25,231 msg/s | 46.940% |
| 평균 | - | - | 31.047% |

managed Buffer를 보유한 Message는 native frame을 공유할 수 없으므로 writable view를 `WeakSet`에
기록해도 send ownership 판단에 사용되지 않는다. native-backed Message만 기록·정리하도록 바꿨다.
이전 채택값 30.62%보다 0.43%p 높아 채택했다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_105440_node-pub-managed-weakset-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_105513_node-pub-managed-weakset.txt`

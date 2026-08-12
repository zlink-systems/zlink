# Node Router unread relay ownership 이동 결과

## 측정 조건

- Core: `0.10.1` release runtime
- 순서: C를 먼저 실행하고 Node를 다음에 실행
- 병렬 실행: 없음
- 대상: `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER`의 `tcp`
- size: 64·256·1024·4096·131072B
- clients: 100, duration: 1초, runs: 1, balanced auto-HWM

## 구현과 측정값

Router single-part receive는 처음에는 native `msg_t`를 Message가 보유하게 한다. application이 `data()`를
읽으면 managed Buffer를 복사해 반환하고 native frame을 해제한다. 읽지 않고 routed send하면 native frame을
Core send 호출로 이동한다. 따라서 relay는 복사를 제거하고, 읽기 경로는 기존 managed Buffer 동작을 유지한다.

| Pattern | Size | C throughput | Node throughput | C 대비 |
|---|---:|---:|---:|---:|
| `MULTI_DEALER_ROUTER` | 64B | 178,006 msg/s | 88,009 msg/s | 49.442% |
| | 256B | 185,656 msg/s | 82,723 msg/s | 44.555% |
| | 1024B | 177,449 msg/s | 84,783 msg/s | 47.780% |
| | 4096B | 163,359 msg/s | 77,305 msg/s | 47.322% |
| | 131072B | 21,644 msg/s | 14,048 msg/s | 64.906% |
| `MULTI_ROUTER_ROUTER` | 64B | 178,312 msg/s | 65,062 msg/s | 36.488% |
| | 256B | 189,478 msg/s | 60,916 msg/s | 32.150% |
| | 1024B | 185,083 msg/s | 59,903 msg/s | 32.366% |
| | 4096B | 169,817 msg/s | 52,135 msg/s | 30.701% |
| | 131072B | 21,310 msg/s | 12,433 msg/s | 58.344% |

| Pattern | C 대비 평균 | 이전 평균 | 변화 |
|---|---:|---:|---:|
| `MULTI_DEALER_ROUTER` | 50.801% | 36.113% | +14.688%p |
| `MULTI_ROUTER_ROUTER` | 38.010% | 36.262% | +1.748%p |

- C DEALER_ROUTER report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_112339_node-router-native-relay-c.txt`
- Node DEALER_ROUTER report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_112432_node-router-native-relay.txt`
- C ROUTER_ROUTER report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_112509_node-router-native-router-router-c.txt`
- Node ROUTER_ROUTER reports: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_112858_node-router-lazy-relay-router-router-64.txt`, `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_113026_node-router-lazy-relay-router-router-256.txt`, `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_113039_node-router-lazy-relay-router-router-1024.txt`, `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_113051_node-router-lazy-relay-router-router-4096.txt`, `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_113104_node-router-lazy-relay-router-router-131072.txt`

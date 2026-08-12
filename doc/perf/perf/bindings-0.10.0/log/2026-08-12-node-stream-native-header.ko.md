# Node STREAM native header materialization 결과

## 측정 조건

- Core: `0.10.1` release runtime
- 순서: C를 먼저 실행하고 Node를 다음에 실행
- 병렬 실행: 없음
- 대상: `MULTI_STREAM / tcp`
- size: 64·256·1024·65536B
- clients: 100, duration: 1초, runs: 1, balanced auto-HWM

## 구현과 측정값

STREAM packet callback은 header와 body를 모두 native `msg_t` frame으로 Message에 전달한다. handler가
`size()`만 사용하거나 두 Message를 send하면 managed Buffer를 만들지 않는다. `data()`를 호출하면 기존과
같은 Buffer view를 제공한다.

| Size | C throughput | Node throughput | C 대비 |
|---:|---:|---:|---:|
| 64B | 324,081 msg/s | 151,764 msg/s | 46.827% |
| 256B | 339,018 msg/s | 129,042 msg/s | 38.063% |
| 1024B | 333,813 msg/s | 150,416 msg/s | 45.059% |
| 65536B | 63,101 msg/s | 56,099 msg/s | 88.903% |
| 평균 | - | - | 54.713% |

이전 STREAM 평균 29.855%보다 24.858%p 개선됐다. callback-owned Message의 public interface와
ownership 규칙은 변경하지 않았다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_113448_node-stream-native-header-final-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_113524_node-stream-native-header-final.txt`

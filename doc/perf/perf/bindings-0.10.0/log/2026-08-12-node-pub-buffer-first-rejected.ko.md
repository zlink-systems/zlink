# Node PUB scalar Buffer-first 후보 결과

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
| 64B | 1,440,735 msg/s | 283,561 msg/s | 19.682% |
| 256B | 1,608,874 msg/s | 320,225 msg/s | 19.904% |
| 1024B | 1,447,740 msg/s | 279,633 msg/s | 19.315% |
| 4096B | 602,655 msg/s | 154,072 msg/s | 25.566% |
| 65536B | 100,686 msg/s | 45,069 msg/s | 44.762% |
| 131072B | 56,038 msg/s | 25,046 msg/s | 44.694% |

throughput ratio 산술평균은 28.987%다. 채택된 scalar Buffer consume 검사 생략 결과의
30.620%보다 낮아 Buffer-first 판별 후보는 원복했다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_095652_node-pub-buffer-first-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_095727_node-pub-buffer-first.txt`

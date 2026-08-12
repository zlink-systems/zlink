# Node ROUTER_ROUTER zero metadata 재측정 결과

## 측정 조건

- Core: `0.10.1` release runtime
- 순서: C를 먼저 실행하고 Node를 다음에 실행
- 병렬 실행: 없음
- 대상: `MULTI_ROUTER_ROUTER / tcp`
- size: 64·256·1024·4096·65536·131072B
- clients: 100, duration: 1초, runs: 1, balanced auto-HWM

## 측정값

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 178,479 ops/s | 50,350 ops/s | 28.211% |
| 256B | 163,099 ops/s | 49,090 ops/s | 30.098% |
| 1024B | 185,169 ops/s | 46,579 ops/s | 25.155% |
| 4096B | 173,580 ops/s | 44,566 ops/s | 25.675% |
| 65536B | 35,432 ops/s | 19,791 ops/s | 55.856% |
| 131072B | 21,554 ops/s | 11,333 ops/s | 52.580% |

throughput ratio 산술평균은 36.262%다. 이전 TCP `MULTI_ROUTER_ROUTER`의 33.630%보다
2.632%p 높다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_095912_node-router-router-zero-meta-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_100013_node-router-router-zero-meta.txt`

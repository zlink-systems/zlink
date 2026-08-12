# Node routed receive zero metadata 생략 결과

## 측정 조건

- Core: `0.10.1` release runtime
- 순서: C를 먼저 실행하고 Node를 다음에 실행
- 병렬 실행: 없음
- 대상: `MULTI_DEALER_ROUTER / tcp`
- size: 64·256·1024·4096·65536·131072B
- clients: 100, duration: 1초, runs: 1, balanced auto-HWM

## 측정값

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 188,758 ops/s | 50,002 ops/s | 26.490% |
| 256B | 184,510 ops/s | 48,802 ops/s | 26.450% |
| 1024B | 179,723 ops/s | 44,912 ops/s | 24.990% |
| 4096B | 168,642 ops/s | 46,446 ops/s | 27.541% |
| 65536B | 37,104 ops/s | 20,151 ops/s | 54.310% |
| 131072B | 22,513 ops/s | 12,810 ops/s | 56.900% |

throughput ratio 산술평균은 36.113%다. 이전 TCP `MULTI_DEALER_ROUTER`의 35.450%보다
0.663%p 높다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_094654_node-router-omit-zero-meta-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_094755_node-router-omit-zero-meta.txt`

# Node STREAM routing-id WeakMap cache 결과

## 측정 조건

- Core: `0.10.1` release runtime
- 순서: C를 먼저 실행하고 Node를 다음에 실행
- 병렬 실행: 없음
- 대상: `MULTI_STREAM / tcp`
- size: 64·256·1024·65536B
- clients: 100, duration: 1초, runs: 1, balanced auto-HWM

## 측정값

| Size | C throughput | Node throughput | Node / C |
|---:|---:|---:|---:|
| 64B | 290,113 ops/s | 63,877 ops/s | 22.018% |
| 256B | 320,481 ops/s | 64,676 ops/s | 20.181% |
| 1024B | 329,527 ops/s | 63,308 ops/s | 19.212% |
| 65536B | 57,078 ops/s | 33,111 ops/s | 58.010% |

throughput ratio 산술평균은 29.855%다. native routing-id cache가 반환하는 동일 `Buffer` object를
`WeakMap<Buffer, RoutingId>` key로 사용해 per-packet Latin-1 string 생성과 `Map<string>` lookup을
제거했다. 이전 29.590%보다 0.265%p 개선되어 채택했다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_104103_node-stream-rid-weakmap-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_104137_node-stream-rid-weakmap.txt`

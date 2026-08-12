# Node STREAM contiguous send 단일 순회 결과

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
| 64B | 344,556 ops/s | 54,637 ops/s | 15.857% |
| 256B | 344,773 ops/s | 51,508 ops/s | 14.940% |
| 1024B | 338,711 ops/s | 51,816 ops/s | 15.296% |
| 65536B | 62,090 ops/s | 39,255 ops/s | 63.222% |

throughput ratio 산술평균은 27.329%다. 이전 inline routing-id storage 결과의 24.340%보다
2.989%p 높다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_095434_node-stream-single-pass-send-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_095508_node-stream-single-pass-send.txt`

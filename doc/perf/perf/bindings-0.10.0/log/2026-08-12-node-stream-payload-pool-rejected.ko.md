# Node STREAM TSFN payload pool 후보 결과

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
| 64B | 336,416 ops/s | 56,081 ops/s | 16.670% |
| 256B | 354,355 ops/s | 59,223 ops/s | 16.713% |
| 1024B | 338,171 ops/s | 60,764 ops/s | 17.968% |
| 65536B | 59,992 ops/s | 38,097 ops/s | 63.503% |

throughput ratio 산술평균은 28.714%다. I/O thread에서 TSFN queue로 넘기는 payload의 `new/delete`를
stream slot별 bounded pool로 재사용했지만, mutex와 반환 비용이 더 커서 현재 채택값 29.590%보다
낮았다. 변경은 원복했다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_103718_node-stream-payload-pool-c.txt`
- Node report: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260812_103753_node-stream-payload-pool.txt`

# Go WSS PUBSUB 측정 결과

| Size | C Kmsg/s | Go Kmsg/s | Go/C |
|---|---:|---:|---:|
| 64B | 1099.602 | 491.796 | 44.72% |
| 256B | 562.471 | 474.592 | 84.38% |
| 1024B | 226.913 | 228.450 | 100.68% |
| 65536B | 9.945 | 8.682 | 87.30% |
| 131072B | 6.099 | 5.001 | 82.00% |
| 262144B | 3.362 | 2.559 | 76.12% |
| 산술 평균 | - | - | **79.20%** |

Core release `0.10.1`, C 뒤 Go 순서, duration `1초`, runs `1`, balanced auto-HWM이다.
simple one-way 중앙값 목표 65%를 충족한다.

- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260812_144404_go-pubsub-wss-current-c.txt`
- Go report: `bindings/go/perf/results/single/report/perf_go_single_linux_20260812_144421_go-pubsub-wss-current-go.txt`

# Go WS MULTI_PUBSUB 측정 결과

| Size | C Kmsg/s | Go Kmsg/s | Go/C |
|---|---:|---:|---:|
| 64B | 979.878 | 4527.676 | 462.07% |
| 256B | 1232.024 | 1729.982 | 140.42% |
| 1024B | 1191.030 | 871.883 | 73.21% |
| 4096B | 531.154 | 396.989 | 74.74% |
| 65536B | 70.002 | 64.755 | 92.50% |
| 131072B | 37.458 | 43.219 | 115.38% |
| 산술 평균 | - | - | **159.72%** |

Core release `0.10.1`, C 뒤 Go 순서, clients `100`, duration `1초`, runs `1`, balanced auto-HWM이다.
simple one-way 중앙값 목표 65%를 충족한다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_144023_go-multi-pubsub-ws-current-c.txt`
- Go report: `bindings/go/perf/results/multi/report/perf_go_multi_linux_20260812_144040_go-multi-pubsub-ws-current-go.txt`

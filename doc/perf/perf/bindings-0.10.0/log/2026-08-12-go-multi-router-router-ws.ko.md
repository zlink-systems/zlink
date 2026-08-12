# Go WS MULTI_ROUTER_ROUTER 측정 결과

| Size | C Kops/s | Go Kops/s | Go/C |
|---|---:|---:|---:|
| 64B | 170.589 | 107.203 | 62.84% |
| 256B | 171.609 | 109.726 | 63.94% |
| 1024B | 160.110 | 104.555 | 65.30% |
| 4096B | 140.407 | 87.104 | 62.04% |
| 65536B | 32.383 | 27.070 | 83.59% |
| 131072B | 16.193 | 14.131 | 87.26% |
| 산술 평균 | - | - | **70.83%** |

Core release `0.10.1`, C 뒤 Go 순서, clients `100`, duration `1초`, runs `1`, balanced auto-HWM이다.
routed one-way 중앙값 목표 57%를 충족한다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_143915_go-multi-router-router-ws-current-c.txt`
- Go report: `bindings/go/perf/results/multi/report/perf_go_multi_linux_20260812_143930_go-multi-router-router-ws-current-go.txt`

# Go WS MULTI_DEALER_ROUTER 측정 결과

| Size | C Kops/s | Go Kops/s | Go/C |
|---|---:|---:|---:|
| 64B | 171.346 | 123.696 | 72.19% |
| 256B | 164.477 | 123.127 | 74.86% |
| 1024B | 161.222 | 123.169 | 76.40% |
| 4096B | 139.257 | 102.047 | 73.28% |
| 65536B | 33.066 | 29.413 | 88.94% |
| 131072B | 16.330 | 15.146 | 92.75% |
| 산술 평균 | - | - | **79.74%** |

Core release `0.10.1`, C 뒤 Go 순서, clients `100`, duration `1초`, runs `1`, balanced auto-HWM이다.
routed one-way 중앙값 목표 57%를 충족한다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_143845_go-multi-dealer-router-ws-current-c.txt`
- Go report: `bindings/go/perf/results/multi/report/perf_go_multi_linux_20260812_143858_go-multi-dealer-router-ws-current-go.txt`

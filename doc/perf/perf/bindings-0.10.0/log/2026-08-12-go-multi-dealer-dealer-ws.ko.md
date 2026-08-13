# Go WS MULTI_DEALER_DEALER 측정 결과

| Size | C Kmsg/s | Go Kmsg/s | Go/C |
|---|---:|---:|---:|
| 64B | 2237.345 | 650.027 | 29.05% |
| 256B | 1454.311 | 786.454 | 54.08% |
| 1024B | 935.538 | 712.691 | 76.18% |
| 4096B | 391.842 | 274.433 | 70.04% |
| 65536B | 50.579 | 54.674 | 108.10% |
| 131072B | 28.118 | 25.593 | 91.02% |
| 산술 평균 | - | - | **71.41%** |

Core release `0.10.1`, C 뒤 Go 순서, clients `100`, duration `1초`, runs `1`, balanced auto-HWM이다.
simple one-way 중앙값 목표 65%를 충족한다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_143810_go-multi-dealer-dealer-ws-current-c.txt`
- Go report: `bindings/go/perf/results/multi/report/perf_go_multi_linux_20260812_143823_go-multi-dealer-dealer-ws-current-go.txt`

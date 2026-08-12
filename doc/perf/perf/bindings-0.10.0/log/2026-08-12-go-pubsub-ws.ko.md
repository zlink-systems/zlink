# Go WS PUBSUB 측정 결과

## 조건

- 대상: `PUBSUB / ws`
- Core: release `0.10.1`
- 순서: C를 단독 실행한 뒤 Go를 단독 실행
- duration: `1초`
- runs: `1`
- HWM: balanced auto-HWM

## 결과

| Size | C Kmsg/s | Go Kmsg/s | Go/C |
|---|---:|---:|---:|
| 64B | 1091.108 | 484.144 | 44.37% |
| 256B | 822.538 | 438.919 | 53.36% |
| 1024B | 399.279 | 416.137 | 104.22% |
| 65536B | 24.990 | 20.544 | 82.21% |
| 131072B | 15.884 | 13.290 | 83.67% |
| 262144B | 10.946 | 8.934 | 81.62% |
| 산술 평균 | - | - | **74.91%** |

산술평균 74.91%는 Go simple one-way 중앙값 목표 65%를 충족한다.

- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260812_143443_go-pubsub-ws-current-c.txt`
- Go report: `bindings/go/perf/results/single/report/perf_go_single_linux_20260812_143505_go-pubsub-ws-current-go.txt`

# Go WS DEALER_ROUTER 측정 결과

## 조건

- 대상: `DEALER_ROUTER / ws`
- Core: release `0.10.1`
- 순서: C를 단독 실행한 뒤 Go를 단독 실행
- duration: `1초`
- runs: `1`
- HWM: balanced auto-HWM

## 결과

| Size | C Kmsg/s | Go Kmsg/s | Go/C |
|---|---:|---:|---:|
| 64B | 1652.192 | 621.645 | 37.62% |
| 256B | 904.096 | 553.097 | 61.17% |
| 1024B | 457.629 | 468.008 | 102.27% |
| 65536B | 22.277 | 22.276 | 99.99% |
| 131072B | 14.990 | 15.362 | 102.48% |
| 262144B | 8.974 | 9.218 | 102.83% |
| 산술 평균 | - | - | **84.39%** |

산술평균 84.39%는 Go routed one-way 중앙값 목표 57%를 충족한다.

- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260812_143636_go-dealer-router-ws-current-c.txt`
- Go report: `bindings/go/perf/results/single/report/perf_go_single_linux_20260812_143647_go-dealer-router-ws-harness-parity-go.txt`

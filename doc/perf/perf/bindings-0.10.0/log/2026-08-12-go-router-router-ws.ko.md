# Go WS ROUTER_ROUTER 측정 결과

## 조건

- 대상: `ROUTER_ROUTER / ws`
- Core: release `0.10.1`
- 순서: C를 단독 실행한 뒤 Go를 단독 실행
- duration: `1초`
- runs: `1`
- HWM: balanced auto-HWM

## 결과

| Size | C Kmsg/s | Go Kmsg/s | Go/C |
|---|---:|---:|---:|
| 64B | 1510.194 | 608.576 | 40.30% |
| 256B | 1003.294 | 511.807 | 51.01% |
| 1024B | 422.781 | 476.302 | 112.66% |
| 65536B | 21.908 | 24.328 | 111.50% |
| 131072B | 14.762 | 16.387 | 111.01% |
| 262144B | 9.109 | 9.729 | 106.81% |
| 산술 평균 | - | - | **88.88%** |

산술평균 88.88%는 Go routed one-way 중앙값 목표 57%를 충족한다.

- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260812_143703_go-router-router-ws-current-c.txt`
- Go report: `bindings/go/perf/results/single/report/perf_go_single_linux_20260812_143718_go-router-router-ws-current-go.txt`

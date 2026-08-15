# Go TCP ROUTER_ROUTER 측정 결과

## 조건

- 대상: `ROUTER_ROUTER / tcp`
- Core: release `0.10.1`
- 순서: C를 단독 실행한 뒤 Go를 단독 실행
- duration: `1초`
- runs: `1`
- HWM: balanced auto-HWM

## 결과

| Size | C Kmsg/s | Go Kmsg/s | Go/C |
|---|---:|---:|---:|
| 64B | 1986.075 | 620.803 | 31.26% |
| 256B | 1175.382 | 590.315 | 50.22% |
| 1024B | 661.720 | 585.307 | 88.45% |
| 65536B | 38.932 | 37.381 | 96.02% |
| 131072B | 25.751 | 24.835 | 96.44% |
| 262144B | 15.381 | 16.261 | 105.72% |
| 산술 평균 | - | - | **78.02%** |

산술평균 78.02%는 Go routed one-way 중앙값 목표 57%를 충족한다.

- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260812_143258_go-router-router-tcp-current-c.txt`
- Go report: `bindings/go/perf/results/single/report/perf_go_single_linux_20260812_143311_go-router-router-tcp-current-go.txt`

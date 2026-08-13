# Go TCP PAIR 측정 결과

## 조건

- 대상: `PAIR / tcp`
- Core: release `0.10.1`
- 순서: C를 단독 실행한 뒤 Go를 단독 실행
- duration: `1초`
- runs: `1`
- HWM: balanced auto-HWM

## 결과

| Size | C Kmsg/s | Go Kmsg/s | Go/C |
|---|---:|---:|---:|
| 64B | 2394.746 | 812.498 | 33.93% |
| 256B | 1178.489 | 782.470 | 66.40% |
| 1024B | 660.258 | 743.347 | 112.59% |
| 65536B | 38.289 | 29.818 | 77.88% |
| 131072B | 24.844 | 19.268 | 77.55% |
| 262144B | 14.836 | 13.514 | 91.09% |
| 산술 평균 | - | - | **76.57%** |

산술평균 76.57%는 Go simple one-way 중앙값 목표 65%를 충족한다.

- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260812_142112_go-pair-tcp-current-c.txt`
- Go report: `bindings/go/perf/results/single/report/perf_go_single_linux_20260812_142127_go-pair-tcp-current-go.txt`

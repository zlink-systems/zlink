# Go TCP PUBSUB 측정 결과

## 조건

- 대상: `PUBSUB / tcp`
- Core: release `0.10.1`
- 순서: C를 단독 실행한 뒤 Go를 단독 실행
- duration: `1초`
- runs: `1`
- HWM: balanced auto-HWM

## 결과

| Size | C Kmsg/s | Go Kmsg/s | Go/C |
|---|---:|---:|---:|
| 64B | 1409.623 | 533.467 | 37.85% |
| 256B | 914.671 | 515.833 | 56.40% |
| 1024B | 584.789 | 544.984 | 93.19% |
| 65536B | 36.076 | 30.428 | 84.34% |
| 131072B | 25.013 | 18.187 | 72.71% |
| 262144B | 14.820 | 11.292 | 76.19% |
| 산술 평균 | - | - | **70.11%** |

산술평균 70.11%는 Go simple one-way 중앙값 목표 65%를 충족한다.

- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260812_142213_go-pubsub-tcp-current-c.txt`
- Go report: `bindings/go/perf/results/single/report/perf_go_single_linux_20260812_142231_go-pubsub-tcp-current-go.txt`

# Go WS DEALER_DEALER 측정 결과

## 조건

- 대상: `DEALER_DEALER / ws`
- Core: release `0.10.1`
- 순서: C를 단독 실행한 뒤 Go를 단독 실행
- duration: `1초`
- runs: `1`
- HWM: balanced auto-HWM

## 결과

| Size | C Kmsg/s | Go Kmsg/s | Go/C |
|---|---:|---:|---:|
| 64B | 1676.590 | 672.631 | 40.12% |
| 256B | 942.978 | 590.214 | 62.59% |
| 1024B | 428.033 | 394.476 | 92.16% |
| 65536B | 21.714 | 17.829 | 82.11% |
| 131072B | 15.028 | 11.651 | 77.53% |
| 262144B | 8.756 | 7.569 | 86.45% |
| 산술 평균 | - | - | **73.49%** |

산술평균 73.49%는 Go simple one-way 중앙값 목표 65%를 충족한다.

- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260812_143550_go-dealer-dealer-ws-current-c.txt`
- Go report: `bindings/go/perf/results/single/report/perf_go_single_linux_20260812_143602_go-dealer-dealer-ws-current-go.txt`

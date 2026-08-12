# Go TCP DEALER_DEALER 측정 결과

## 조건

- 대상: `DEALER_DEALER / tcp`
- Core: release `0.10.1`
- 순서: C를 단독 실행한 뒤 Go를 단독 실행
- duration: `1초`
- runs: `1`
- HWM: balanced auto-HWM

## 결과

| Size | C Kmsg/s | Go Kmsg/s | Go/C |
|---|---:|---:|---:|
| 64B | 1346.445 | 619.375 | 46.00% |
| 256B | 1069.684 | 602.579 | 56.33% |
| 1024B | 630.509 | 543.566 | 86.21% |
| 65536B | 34.775 | 21.676 | 62.34% |
| 131072B | 20.556 | 12.946 | 62.98% |
| 262144B | 11.879 | 8.581 | 72.24% |
| 산술 평균 | - | - | **64.35%** |

산술평균은 Go simple one-way 중앙값 목표 65%에 0.65%p 미달한다. 앞서 적용한
single-part builder의 slice allocation 제거가 포함된 결과다. 남은 일반 send builder와
cgo 호출 비용은 public builder의 독립 lifetime과 ownership 규칙을 유지하면서 이 패턴만
분리해 줄일 수 없으므로, 추가 변경 없이 측정값으로 기록한다.

- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260812_142814_go-dealer-dealer-tcp-current-c.txt`
- Go report: `bindings/go/perf/results/single/report/perf_go_single_linux_20260812_142827_go-dealer-dealer-tcp-current-go.txt`

# Go TCP DEALER_DEALER single-part builder 결과

## 조건

- 대상: `MULTI_DEALER_DEALER / tcp`
- Core: release `0.10.1`
- 순서: C를 단독 실행한 뒤 Go를 단독 실행
- clients: `100`
- duration: `1초`
- runs: `1`
- HWM: balanced auto-HWM

## 변경

첫 single-part frame은 `sendBuilder` 내부 고정 배열에 보관한다. 두 번째 frame부터 기존
multipart slice를 사용한다. 따라서 일반 single-part send의 builder allocation을 없애고,
multipart와 public interface는 기존 경로를 유지한다.

## 결과

| Size | C Kmsg/s | Go Kmsg/s | Go/C |
|---|---:|---:|---:|
| 64B | 2739.702 | 886.488 | 32.36% |
| 256B | 1425.961 | 867.852 | 60.86% |
| 1024B | 1030.134 | 825.459 | 80.13% |
| 4096B | 315.663 | 168.347 | 53.33% |
| 65536B | 94.328 | 91.533 | 97.04% |
| 131072B | 46.425 | 43.423 | 93.53% |
| 산술 평균 | - | - | **69.54%** |

산술평균 69.54%는 Go simple one-way 중앙값 목표 65%를 충족한다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_140939_go-dealer-dealer-tcp-builder-c.txt`
- Go report: `bindings/go/perf/results/multi/report/perf_go_multi_linux_20260812_140955_go-dealer-dealer-tcp-builder-go.txt`

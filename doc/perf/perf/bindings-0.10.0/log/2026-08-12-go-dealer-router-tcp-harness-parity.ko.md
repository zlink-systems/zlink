# Go TCP DEALER_ROUTER harness parity 및 측정 결과

## 수정

기존 Go harness는 ROUTER와 DEALER monitor를 순서대로 block하며 대기했다. `DEALER_ROUTER`
에서는 ROUTER가 socket activity를 처리하는 동안 연결 상태가 확정되므로, 이 방식은 모든 size에서
ready timeout을 만들었다.

C harness 및 Go `ROUTER_ROUTER`와 같이 ROUTER poller를 사용하면서 두 monitor를 nonblocking으로
확인하도록 변경했다. public interface와 binding runtime은 변경하지 않았다.

## 조건

- 대상: `DEALER_ROUTER / tcp`
- Core: release `0.10.1`
- 순서: C를 단독 실행한 뒤 Go를 단독 실행
- duration: `1초`
- runs: `1`
- HWM: balanced auto-HWM

## 결과

| Size | C Kmsg/s | Go Kmsg/s | Go/C |
|---|---:|---:|---:|
| 64B | 2372.144 | 780.476 | 32.90% |
| 256B | 1178.820 | 746.409 | 63.32% |
| 1024B | 702.952 | 708.395 | 100.77% |
| 65536B | 38.173 | 37.381 | 97.92% |
| 131072B | 25.239 | 25.858 | 102.45% |
| 262144B | 15.135 | 16.899 | 111.65% |
| 산술 평균 | - | - | **84.84%** |

산술평균 84.84%는 Go routed one-way 중앙값 목표 57%를 충족한다.

- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260812_143002_go-dealer-router-tcp-current-c.txt`
- Go failure report (수정 전): `bindings/go/perf/results/single/report/perf_go_single_linux_20260812_143017_go-dealer-router-tcp-current-go.txt`
- Go final report: `bindings/go/perf/results/single/report/perf_go_single_linux_20260812_143204_go-dealer-router-tcp-harness-parity-go.txt`

# Go TCP Multi pattern 측정 결과

## 공통 조건

- Core: release `0.10.1`
- 순서: 각 pattern에서 C를 단독 실행한 뒤 Go를 단독 실행
- clients: `100`
- duration: `1초`
- runs: `1`
- HWM: balanced auto-HWM
- Go package runtime SHA-256: `3fb820960bba7798ebcc1c5e256ba092fa1a018664b63b97d380e04aa9d14e6c`

## 결과

| Pattern | 64B | 256B | 1024B | 4096B | 65536B | 131072B | 산술 평균 | 판정 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| `MULTI_DEALER_DEALER` | 32.36% | 60.86% | 80.13% | 53.33% | 97.04% | 93.53% | 69.54% | simple one-way 중앙값 목표 65% 통과 |
| `MULTI_DEALER_ROUTER` | 75.46% | 77.89% | 73.39% | 82.83% | 101.39% | 99.51% | 85.08% | routed one-way 중앙값 목표 57% 통과 |
| `MULTI_ROUTER_ROUTER` | 66.03% | 65.71% | 62.63% | 63.52% | 96.13% | 101.38% | 75.90% | routed one-way 중앙값 목표 57% 통과 |
| `MULTI_PUBSUB` | 237.78% | 127.23% | 79.84% | 93.02% | 109.23% | 139.39% | 131.08% | simple one-way 중앙값 목표 65% 통과 |

`MULTI_DEALER_DEALER`는 single-part builder가 첫 frame을 내부 고정 배열에 보관하도록 변경한
뒤 측정했다. 나머지 세 pattern은 같은 변경을 포함한 상태에서 별도 코드 변경 없이 측정했다.

## 결과 파일

- DEALER_DEALER C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_140939_go-dealer-dealer-tcp-builder-c.txt`
- DEALER_DEALER Go: `bindings/go/perf/results/multi/report/perf_go_multi_linux_20260812_140955_go-dealer-dealer-tcp-builder-go.txt`
- DEALER_ROUTER C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_141107_go-dealer-router-tcp-current-c.txt`
- DEALER_ROUTER Go: `bindings/go/perf/results/multi/report/perf_go_multi_linux_20260812_141121_go-dealer-router-tcp-current-go.txt`
- ROUTER_ROUTER C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_141146_go-router-router-tcp-current-c.txt`
- ROUTER_ROUTER Go: `bindings/go/perf/results/multi/report/perf_go_multi_linux_20260812_141159_go-router-router-tcp-current-go.txt`
- PUBSUB C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_141215_go-pubsub-tcp-current-c.txt`
- PUBSUB Go: `bindings/go/perf/results/multi/report/perf_go_multi_linux_20260812_141226_go-pubsub-tcp-current-go.txt`

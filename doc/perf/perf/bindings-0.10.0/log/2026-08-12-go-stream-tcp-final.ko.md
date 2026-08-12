# Go TCP STREAM 측정 결과

## 조건

- 대상: `MULTI_STREAM / tcp`
- Core: release `0.10.1`
- 순서: C를 단독 실행한 뒤 Go를 단독 실행
- clients: `100`
- duration: `1초`
- runs: `1`
- HWM: balanced auto-HWM

## 최종 측정

| Size | C Kops/s | Go Kops/s | Go/C |
|---|---:|---:|---:|
| 64B | 319.104 | 160.734 | 50.37% |
| 256B | 319.491 | 161.010 | 50.40% |
| 1024B | 316.169 | 127.337 | 40.27% |
| 65536B | 53.664 | 21.392 | 39.86% |
| 산술 평균 | - | - | **45.22%** |

산술평균은 multi routed echo 최소 기준 40%를 충족하지만 중앙값 목표 53%에는 미달한다.

## 후보 A/B

header/body를 Go byte slice에 먼저 만들지 않고 native `Message`에 직접 채우는 후보를 비교했다.
동일 시점의 Go-only baseline보다 64B·256B·1024B throughput이 낮아 채택하지 않았고, 기존 구현을
유지한다.

| Variant | 64B Kops/s | 256B Kops/s | 1024B Kops/s | 65536B Kops/s |
|---|---:|---:|---:|---:|
| 기존 frame | 159.499 | 145.006 | 122.249 | 22.061 |
| native frame 직접 생성 | 96.817 | 103.963 | 97.013 | 22.772 |

- 최종 C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_141924_go-stream-tcp-final-c.txt`
- 최종 Go report: `bindings/go/perf/results/multi/report/perf_go_multi_linux_20260812_141933_go-stream-tcp-final-go.txt`
- candidate Go report: `bindings/go/perf/results/multi/report/perf_go_multi_linux_20260812_141805_go-stream-tcp-direct-frame-go.txt`
- baseline Go report: `bindings/go/perf/results/multi/report/perf_go_multi_linux_20260812_141854_go-stream-tcp-frame-baseline-current-go.txt`

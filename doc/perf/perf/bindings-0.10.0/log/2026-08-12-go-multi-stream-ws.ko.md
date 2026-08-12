# Go WS MULTI_STREAM 측정 결과

기본 stream clients `10000`은 WSL 메모리 guard에 걸려 skip됐다. 다른 WS multi pattern과 같은
`100` clients를 C와 Go에 명시했다.

| Size | C Kops/s | Go Kops/s | Go/C |
|---|---:|---:|---:|
| 64B | 296.101 | 153.085 | 51.70% |
| 256B | 294.450 | 154.030 | 52.31% |
| 1024B | 277.008 | 143.534 | 51.82% |
| 65536B | 16.190 | 14.873 | 91.87% |
| 산술 평균 | - | - | **61.93%** |

Core release `0.10.1`, C 뒤 Go 순서, clients `100`, duration `1초`, runs `1`, balanced auto-HWM이다.
multi routed echo 중앙값 목표 53%를 충족한다.

- 기본 clients skip: C runner `MULTI_STREAM / ws`, guard `max_clients=6468`
- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_144149_go-multi-stream-ws-100-current-c.txt`
- Go report: `bindings/go/perf/results/multi/report/perf_go_multi_linux_20260812_144158_go-multi-stream-ws-100-current-go.txt`

# Go WSS PAIR 측정 결과

| Size | C Kmsg/s | Go Kmsg/s | Go/C |
|---|---:|---:|---:|
| 64B | 1663.608 | 754.111 | 45.33% |
| 256B | 706.894 | 445.160 | 62.97% |
| 1024B | 234.488 | 225.854 | 96.32% |
| 65536B | 9.386 | 8.049 | 85.76% |
| 131072B | 5.437 | 4.813 | 88.53% |
| 262144B | 3.106 | 2.623 | 84.45% |
| 산술 평균 | - | - | **77.23%** |

Core release `0.10.1`, C 뒤 Go 순서, duration `1초`, runs `1`, balanced auto-HWM이다.
simple one-way 중앙값 목표 65%를 충족한다.

- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260812_144319_go-pair-wss-current-c.txt`
- Go report: `bindings/go/perf/results/single/report/perf_go_single_linux_20260812_144330_go-pair-wss-current-go.txt`

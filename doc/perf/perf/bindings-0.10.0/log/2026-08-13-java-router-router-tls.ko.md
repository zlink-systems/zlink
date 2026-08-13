# Java ROUTER/ROUTER send/send TLS 결과

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 151,386.0 | 127,488.0 | 84.21% |
| 256B | 164,824.5 | 120,227.0 | 72.94% |
| 1024B | 155,724.5 | 112,131.0 | 72.01% |
| 4096B | 120,493.5 | 92,552.5 | 76.81% |
| 65536B | 18,569.5 | 16,820.5 | 90.58% |
| 131072B | 9,345.5 | 8,483.5 | 90.78% |
| 산술평균 | - | - | 81.22% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Java 22.0.2
- 대상: `MULTI_ROUTER_ROUTER_SENDSEND / tls`, balanced auto-HWM
- C report: `/tmp/zlink-java-rr-tls-c/multi/report/perf_c_multi_linux_20260813_230235_java-rr-tls-c.txt`
- Java report: `/tmp/zlink-java-rr-tls-java/multi/report/perf_java_multi_linux_20260813_230302_java-rr-tls-java.txt`

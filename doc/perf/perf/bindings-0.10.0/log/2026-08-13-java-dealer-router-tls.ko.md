# Java DEALER/ROUTER send/send TLS 결과

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 152,232.0 | 129,151.0 | 84.84% |
| 256B | 119,163.5 | 125,331.5 | 105.18% |
| 1024B | 144,380.5 | 114,278.5 | 79.15% |
| 4096B | 122,810.5 | 96,301.5 | 78.41% |
| 65536B | 17,725.0 | 17,190.0 | 96.98% |
| 131072B | 9,610.0 | 8,956.0 | 93.19% |
| 산술평균 | - | - | 89.63% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Java 22.0.2
- 대상: `MULTI_DEALER_ROUTER_SENDSEND / tls`, balanced auto-HWM
- C report: `/tmp/zlink-java-dr-tls-c/multi/report/perf_c_multi_linux_20260813_225442_java-dr-tls-c.txt`
- Java report: `/tmp/zlink-java-dr-tls-java/multi/report/perf_java_multi_linux_20260813_225512_java-dr-tls-java.txt`

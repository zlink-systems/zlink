# Java STREAM TLS 결과

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 252,441.0 | 204,469.0 | 80.96% |
| 256B | 237,604.0 | 199,767.5 | 84.08% |
| 1024B | 216,822.5 | 182,001.5 | 83.94% |
| 65536B | 19,363.5 | 18,308.0 | 94.55% |
| 산술평균 | - | - | 85.88% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Java 22.0.2
- 대상: `MULTI_STREAM / tls`, balanced auto-HWM
- C report: `/tmp/zlink-java-stream-tls-c/multi/report/perf_c_multi_linux_20260813_231657_java-stream-tls-c.txt`
- Java report: `/tmp/zlink-java-stream-tls-java/multi/report/perf_java_multi_linux_20260813_231712_java-stream-tls-java.txt`

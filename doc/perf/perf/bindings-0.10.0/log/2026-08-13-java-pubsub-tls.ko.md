# Java PUB/SUB TLS 결과

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 1,237,814.5 | 933,515.0 | 75.42% |
| 256B | 1,491,905.0 | 925,688.0 | 62.05% |
| 1024B | 1,068,191.0 | 838,163.0 | 78.47% |
| 4096B | 390,866.0 | 240,455.5 | 61.52% |
| 65536B | 34,587.0 | 33,331.5 | 96.37% |
| 131072B | 19,299.5 | 14,463.5 | 74.94% |
| 산술평균 | - | - | 74.80% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Java 22.0.2
- 대상: `MULTI_PUBSUB / tls`, balanced auto-HWM
- C report: `/tmp/zlink-java-pubsub-tls-c-rerun/multi/report/perf_c_multi_linux_20260813_230702_java-pubsub-tls-c-rerun.txt`
- Java report: `/tmp/zlink-java-pubsub-tls-java/multi/report/perf_java_multi_linux_20260813_230725_java-pubsub-tls-java.txt`

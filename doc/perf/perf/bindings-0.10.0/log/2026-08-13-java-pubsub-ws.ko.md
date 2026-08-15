# Java PUB/SUB WS 결과

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 1,208,993.5 | 803,280.5 | 66.44% |
| 256B | 1,408,709.5 | 864,892.5 | 61.40% |
| 1024B | 1,177,325.5 | 581,762.5 | 49.41% |
| 4096B | 504,194.5 | 236,201.0 | 46.85% |
| 65536B | 53,957.5 | 31,937.5 | 59.19% |
| 131072B | 30,286.0 | 23,223.5 | 76.68% |
| 산술평균 | - | - | 59.99% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Java 22.0.2
- 대상: `MULTI_PUBSUB / ws`, balanced auto-HWM
- C report: `/tmp/zlink-java-pubsub-ws-c/multi/report/perf_c_multi_linux_20260813_230755_java-pubsub-ws-c.txt`
- Java report: `/tmp/zlink-java-pubsub-ws-java/multi/report/perf_java_multi_linux_20260813_230814_java-pubsub-ws-java.txt`

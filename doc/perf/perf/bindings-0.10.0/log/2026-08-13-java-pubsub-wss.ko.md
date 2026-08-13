# Java PUB/SUB WSS 결과

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 800,697.0 | 257,380.5 | 32.14% |
| 256B | 741,629.0 | 403,919.5 | 54.46% |
| 1024B | 489,993.0 | 268,339.5 | 54.76% |
| 4096B | 126,889.5 | 128,938.0 | 101.61% |
| 65536B | 15,451.5 | 15,901.0 | 102.91% |
| 131072B | 9,878.5 | 8,946.5 | 90.57% |
| 산술평균 | - | - | 72.74% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Java 22.0.2
- 대상: `MULTI_PUBSUB / wss`, balanced auto-HWM
- C report: `/tmp/zlink-java-pubsub-wss-c/multi/report/perf_c_multi_linux_20260813_230843_java-pubsub-wss-c.txt`
- Java report: `/tmp/zlink-java-pubsub-wss-java/multi/report/perf_java_multi_linux_20260813_230904_java-pubsub-wss-java.txt`

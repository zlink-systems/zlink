# Java DEALER/ROUTER request/reply WSS 결과

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 93,817.5 | 40,855.5 | 43.55% |
| 256B | 91,644.5 | 36,828.5 | 40.19% |
| 1024B | 84,789.5 | 35,670.0 | 42.07% |
| 4096B | 69,318.0 | 31,376.5 | 45.26% |
| 65536B | 12,347.5 | 6,766.0 | 54.80% |
| 131072B | 6,118.0 | 4,127.5 | 67.46% |
| 산술평균 | - | - | 48.89% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Java 22.0.2
- 대상: `MULTI_DEALER_ROUTER_REQREP / wss`, balanced auto-HWM
- C report: `/tmp/zlink-java-drreq-socketpath-wss-c/multi/report/perf_c_multi_linux_20260813_234003_java-drreq-socketpath-wss-c.txt`
- Java report: `/tmp/zlink-java-drreq-socketpath-wss-java/multi/report/perf_java_multi_linux_20260813_234037_java-drreq-socketpath-wss-java.txt`

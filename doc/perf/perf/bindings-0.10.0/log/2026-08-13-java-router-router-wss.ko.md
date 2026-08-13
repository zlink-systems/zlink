# Java ROUTER/ROUTER send/send WSS 결과

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 139,971.0 | 107,863.0 | 77.06% |
| 256B | 127,374.5 | 74,053.0 | 58.14% |
| 1024B | 126,462.0 | 58,522.0 | 46.28% |
| 4096B | 99,256.0 | 43,725.0 | 44.05% |
| 65536B | 13,025.5 | 8,257.0 | 63.39% |
| 131072B | 6,457.0 | 4,535.0 | 70.23% |
| 산술평균 | - | - | 59.86% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Java 22.0.2
- 대상: `MULTI_ROUTER_ROUTER_SENDSEND / wss`, balanced auto-HWM
- C report: `/tmp/zlink-java-rr-wss-c/multi/report/perf_c_multi_linux_20260813_230456_java-rr-wss-c.txt`
- Java report: `/tmp/zlink-java-rr-wss-java/multi/report/perf_java_multi_linux_20260813_230523_java-rr-wss-java.txt`

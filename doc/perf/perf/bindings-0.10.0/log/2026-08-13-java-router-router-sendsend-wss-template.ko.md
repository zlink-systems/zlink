# Java ROUTER/ROUTER send/send WSS 결과

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 140,064.0 | 106,084.0 | 75.74% |
| 256B | 140,712.0 | 100,857.5 | 71.68% |
| 1024B | 139,834.5 | 93,743.0 | 67.04% |
| 4096B | 103,240.5 | 67,843.5 | 65.71% |
| 65536B | 13,223.5 | 11,192.0 | 84.64% |
| 131072B | 7,897.0 | 6,542.5 | 82.85% |
| 산술평균 | - | - | 74.61% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Java 22.0.2
- 대상: `MULTI_ROUTER_ROUTER_SENDSEND / wss`, balanced auto-HWM
- C report: `/tmp/zlink-java-rrss-template-wss-c/multi/report/perf_c_multi_linux_20260813_235151_java-rrss-template-wss-c.txt`
- Java report: `/tmp/zlink-java-rrss-template2-wss-java/multi/report/perf_java_multi_linux_20260813_235349_java-rrss-template2-wss-java.txt`

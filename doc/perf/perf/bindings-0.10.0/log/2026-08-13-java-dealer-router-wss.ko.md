# Java DEALER/ROUTER send/send WSS 결과

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 139,465.0 | 113,540.0 | 81.41% |
| 256B | 138,538.0 | 113,964.0 | 82.26% |
| 1024B | 131,181.0 | 106,113.0 | 80.89% |
| 4096B | 103,073.0 | 77,393.0 | 75.09% |
| 65536B | 13,846.5 | 11,796.0 | 85.19% |
| 131072B | 7,470.0 | 6,373.5 | 85.32% |
| 산술평균 | - | - | 81.69% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Java 22.0.2
- 대상: `MULTI_DEALER_ROUTER_SENDSEND / wss`, balanced auto-HWM
- C report: `/tmp/zlink-java-dr-wss-c/multi/report/perf_c_multi_linux_20260813_225903_java-dr-wss-c.txt`
- Java report: `/tmp/zlink-java-dr-wss-java/multi/report/perf_java_multi_linux_20260813_225931_java-dr-wss-java.txt`

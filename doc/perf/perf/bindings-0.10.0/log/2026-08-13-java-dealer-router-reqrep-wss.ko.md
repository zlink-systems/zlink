# Java DEALER/ROUTER request/reply WSS 결과

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 90,165.0 | 57,972.5 | 64.30% |
| 256B | 95,198.0 | 58,375.0 | 61.32% |
| 1024B | 89,434.0 | 60,069.5 | 67.17% |
| 4096B | 68,899.0 | 47,302.0 | 68.65% |
| 65536B | 12,741.5 | 9,901.0 | 77.71% |
| 131072B | 7,329.0 | 5,091.0 | 69.46% |
| 산술평균 | - | - | 68.10% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Java 22.0.2
- 대상: `MULTI_DEALER_ROUTER_REQREP / wss`, balanced auto-HWM
- C report: `/tmp/zlink-java-drreq-template-wss-c/multi/report/perf_c_multi_linux_20260813_234732_java-drreq-template-wss-c.txt`
- Java report: `/tmp/zlink-java-drreq-template-wss-java/multi/report/perf_java_multi_linux_20260813_234810_java-drreq-template-wss-java.txt`

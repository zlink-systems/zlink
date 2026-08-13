# Java DEALER/ROUTER request/reply WS 결과

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 102,359.5 | 68,846.0 | 67.26% |
| 256B | 95,950.0 | 68,178.0 | 71.06% |
| 1024B | 96,031.5 | 65,791.0 | 68.51% |
| 4096B | 84,194.0 | 55,881.5 | 66.37% |
| 65536B | 24,900.5 | 19,959.0 | 80.16% |
| 131072B | 15,283.0 | 12,045.5 | 78.82% |
| 산술평균 | - | - | 72.03% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Java 22.0.2
- 대상: `MULTI_DEALER_ROUTER_REQREP / ws`, balanced auto-HWM
- C report: `/tmp/zlink-java-drreq-socketpath-ws-c/multi/report/perf_c_multi_linux_20260813_233907_java-drreq-socketpath-ws-c.txt`
- Java report: `/tmp/zlink-java-drreq-socketpath-ws-java/multi/report/perf_java_multi_linux_20260813_233937_java-drreq-socketpath-ws-java.txt`

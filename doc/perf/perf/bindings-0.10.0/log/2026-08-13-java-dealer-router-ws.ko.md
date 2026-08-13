# Java DEALER/ROUTER send/send WS 결과

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 178,605.5 | 148,578.0 | 83.19% |
| 256B | 179,110.0 | 136,119.0 | 76.00% |
| 1024B | 166,534.0 | 130,064.0 | 78.10% |
| 4096B | 147,466.0 | 107,394.5 | 72.83% |
| 65536B | 34,956.0 | 33,814.0 | 96.73% |
| 131072B | 17,671.5 | 16,972.0 | 96.04% |
| 산술평균 | - | - | 83.81% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Java 22.0.2
- 대상: `MULTI_DEALER_ROUTER_SENDSEND / ws`, balanced auto-HWM
- C report: `/tmp/zlink-java-dr-ws-c/multi/report/perf_c_multi_linux_20260813_225657_java-dr-ws-c.txt`
- Java report: `/tmp/zlink-java-dr-ws-java/multi/report/perf_java_multi_linux_20260813_225722_java-dr-ws-java.txt`

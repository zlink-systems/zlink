# Java DEALER/DEALER WS 결과

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 2,775,604.0 | 1,663,472.5 | 59.93% |
| 256B | 1,570,732.0 | 1,304,701.0 | 83.06% |
| 1024B | 969,094.0 | 957,306.0 | 98.78% |
| 4096B | 491,781.0 | 393,557.0 | 80.03% |
| 65536B | 57,525.0 | 67,582.5 | 117.48% |
| 131072B | 35,248.5 | 31,354.5 | 88.95% |
| 산술평균 | - | - | 88.04% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Java 22.0.2
- 대상: `MULTI_DEALER_DEALER / ws`, balanced auto-HWM
- C report: `/tmp/zlink-java-dd-ws-c/multi/report/perf_c_multi_linux_20260813_225055_java-dd-ws-c.txt`
- Java report: `/tmp/zlink-java-dd-ws-java/multi/report/perf_java_multi_linux_20260813_225127_java-dd-ws-java.txt`

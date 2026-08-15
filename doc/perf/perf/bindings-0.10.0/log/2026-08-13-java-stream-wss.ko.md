# Java STREAM WSS 결과

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 222,036.5 | 167,683.0 | 75.52% |
| 256B | 217,269.5 | 182,419.0 | 83.96% |
| 1024B | 196,116.5 | 161,864.5 | 82.54% |
| 65536B | 11,121.5 | 9,513.0 | 85.54% |
| 산술평균 | - | - | 81.89% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Java 22.0.2
- 대상: `MULTI_STREAM / wss`, balanced auto-HWM
- C report: `/tmp/zlink-java-stream-wss-c/multi/report/perf_c_multi_linux_20260813_231816_java-stream-wss-c.txt`
- Java report: `/tmp/zlink-java-stream-wss-java/multi/report/perf_java_multi_linux_20260813_231831_java-stream-wss-java.txt`

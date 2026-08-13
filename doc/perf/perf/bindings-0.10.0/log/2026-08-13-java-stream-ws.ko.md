# Java STREAM WS 결과

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 317,574.0 | 258,321.0 | 81.34% |
| 256B | 312,237.5 | 234,569.0 | 75.13% |
| 1024B | 287,293.0 | 239,665.0 | 83.42% |
| 65536B | 19,646.5 | 18,717.0 | 95.27% |
| 산술평균 | - | - | 83.79% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Java 22.0.2
- 대상: `MULTI_STREAM / ws`, balanced auto-HWM
- C report: `/tmp/zlink-java-stream-ws-c/multi/report/perf_c_multi_linux_20260813_231736_java-stream-ws-c.txt`
- Java report: `/tmp/zlink-java-stream-ws-java/multi/report/perf_java_multi_linux_20260813_231752_java-stream-ws-java.txt`

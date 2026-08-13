# Java DEALER/DEALER WSS 결과

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 2,670,327.0 | 1,708,959.5 | 64.00% |
| 256B | 1,326,944.5 | 1,172,832.5 | 88.39% |
| 1024B | 624,447.5 | 632,338.0 | 101.26% |
| 4096B | 241,010.5 | 210,802.5 | 87.47% |
| 65536B | 24,917.0 | 22,231.0 | 89.22% |
| 131072B | 13,060.0 | 11,029.5 | 84.45% |
| 산술평균 | - | - | 85.80% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Java 22.0.2
- 대상: `MULTI_DEALER_DEALER / wss`, balanced auto-HWM
- C report: `/tmp/zlink-java-dd-wss-c/multi/report/perf_c_multi_linux_20260813_225245_java-dd-wss-c.txt`
- Java report: `/tmp/zlink-java-dd-wss-java/multi/report/perf_java_multi_linux_20260813_225319_java-dd-wss-java.txt`

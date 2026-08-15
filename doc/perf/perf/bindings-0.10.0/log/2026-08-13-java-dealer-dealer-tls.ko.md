# Java DEALER/DEALER TLS 결과

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 2,746,104.5 | 1,496,470.5 | 54.49% |
| 256B | 1,340,841.0 | 1,239,197.5 | 92.42% |
| 1024B | 879,129.5 | 815,944.0 | 92.81% |
| 4096B | 383,920.5 | 319,029.0 | 83.10% |
| 65536B | 38,288.5 | 34,107.5 | 89.08% |
| 131072B | 20,122.0 | 16,324.0 | 81.13% |
| 산술평균 | - | - | 82.17% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Java 22.0.2
- 대상: `MULTI_DEALER_DEALER / tls`, balanced auto-HWM
- C report: `/tmp/zlink-java-dd-tls-c/multi/report/perf_c_multi_linux_20260813_224847_java-dd-tls-c.txt`
- Java report: `/tmp/zlink-java-dd-tls-java/multi/report/perf_java_multi_linux_20260813_224920_java-dd-tls-java.txt`

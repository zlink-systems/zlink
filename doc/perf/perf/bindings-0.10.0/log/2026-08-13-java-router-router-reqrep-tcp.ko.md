# Java ROUTER/ROUTER request/reply TCP 결과

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 89,700.5 | 40,575.5 | 45.23% |
| 256B | 77,060.5 | 33,478.0 | 43.44% |
| 1024B | 71,499.5 | 31,356.5 | 43.86% |
| 4096B | 69,110.5 | 35,233.0 | 50.98% |
| 65536B | 18,533.5 | 19,092.0 | 103.01% |
| 131072B | 12,928.5 | 12,231.0 | 94.61% |
| 산술평균 | - | - | 63.52% |

- Core: release `0.10.1`
- 대상: `MULTI_ROUTER_ROUTER_REQREP / tcp`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 Java 실행, 병렬 실행 없음
- C report: `/tmp/zlink-java-rr-rr-c/multi/report/perf_c_multi_linux_20260813_215719_java-rr-rr-c.txt`
- Java report: `/tmp/zlink-java-rr-rr-java/multi/report/perf_java_multi_linux_20260813_215740_java-rr-rr-java.txt`

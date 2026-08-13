# Java ROUTER/ROUTER send/send TCP 결과

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 168,167.5 | 107,904.5 | 64.16% |
| 256B | 139,542.5 | 90,343.0 | 64.74% |
| 1024B | 119,182.5 | 86,206.5 | 72.33% |
| 4096B | 118,614.5 | 80,219.5 | 67.63% |
| 65536B | 27,413.5 | 33,943.0 | 123.82% |
| 131072B | 15,373.5 | 21,851.5 | 142.14% |
| 산술평균 | - | - | 89.14% |

- Core: release `0.10.1`
- 대상: `MULTI_ROUTER_ROUTER_SENDSEND / tcp`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 Java 실행, 병렬 실행 없음
- C report: `/tmp/zlink-java-rr-ss-c/multi/report/perf_c_multi_linux_20260813_215625_java-rr-ss-c.txt`
- Java report: `/tmp/zlink-java-rr-ss-java/multi/report/perf_java_multi_linux_20260813_215647_java-rr-ss-java.txt`

# Java ROUTER/ROUTER send/send WS 결과

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 196,769.0 | 121,379.0 | 61.69% |
| 256B | 193,278.0 | 121,441.0 | 62.83% |
| 1024B | 179,874.0 | 124,261.0 | 69.08% |
| 4096B | 144,231.5 | 100,310.0 | 69.55% |
| 65536B | 34,877.5 | 30,490.0 | 87.42% |
| 131072B | 18,094.0 | 16,352.5 | 90.38% |
| 산술평균 | - | - | 73.49% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Java 22.0.2
- 대상: `MULTI_ROUTER_ROUTER_SENDSEND / ws`, balanced auto-HWM
- C report: `/tmp/zlink-java-rr-ws-c/multi/report/perf_c_multi_linux_20260813_230408_java-rr-ws-c.txt`
- Java report: `/tmp/zlink-java-rr-ws-java/multi/report/perf_java_multi_linux_20260813_230428_java-rr-ws-java.txt`

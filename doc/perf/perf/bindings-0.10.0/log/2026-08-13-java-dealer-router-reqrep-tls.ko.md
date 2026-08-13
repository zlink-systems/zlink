# Java DEALER/ROUTER request/reply TLS 결과

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 95,171.0 | 63,861.5 | 67.10% |
| 256B | 101,191.5 | 59,294.5 | 58.60% |
| 1024B | 94,729.5 | 61,362.5 | 64.78% |
| 4096B | 82,299.5 | 56,852.5 | 69.08% |
| 65536B | 8,184.0 | 13,364.5 | 163.30% |
| 131072B | 9,491.5 | 7,762.5 | 81.78% |
| 산술평균 | - | - | 84.11% |

- Core: release `0.10.1`, clients `100`, duration `2초`, runs `1`, serial C then Java 22.0.2
- 대상: `MULTI_DEALER_ROUTER_REQREP / tls`, balanced auto-HWM
- C report: `/tmp/zlink-java-drreq-socketpath-tls-c/multi/report/perf_c_multi_linux_20260813_233800_java-drreq-socketpath-tls-c.txt`
- Java report: `/tmp/zlink-java-drreq-socketpath-tls-java/multi/report/perf_java_multi_linux_20260813_233836_java-drreq-socketpath-tls-java.txt`

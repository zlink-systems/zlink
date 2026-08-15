# Java Dealer/Router request/reply native poller 재측정

- Core: release `0.10.1`
- 실행 순서: C 후 Java, 병렬 실행 없음
- 공통 조건: `MULTI_DEALER_ROUTER_REQREP`, tcp, clients `100`, duration `2초`,
  runs `1`, I/O threads `4/4`, balanced auto-HWM, send/receive timeout `200ms`,
  connect-ready timeout `10000ms`
- C report: `/tmp/zlink-java-dr-reqrep-native-poller-c/multi/report/perf_c_multi_linux_20260813_051109_java-dr-reqrep-native-poller-c.txt`
- Java report: `/tmp/zlink-java-dr-reqrep-native-poller-java/multi/report/perf_java_multi_linux_20260813_051138_java-dr-reqrep-native-poller-java.txt`

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 102,080 ops/s | 47,890 ops/s | 46.92% |
| 256B | 96,198 ops/s | 51,315 ops/s | 53.34% |
| 1024B | 92,722 ops/s | 46,381 ops/s | 50.02% |
| 4096B | 83,737 ops/s | 35,591 ops/s | 42.50% |
| 65536B | 22,779 ops/s | 15,868 ops/s | 69.66% |
| 131072B | 14,901 ops/s | 12,058 ops/s | 80.92% |

산술평균은 `57.23%`다. 두 report는 모두 `status: complete`, result line `30/30`이다.

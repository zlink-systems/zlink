# Java PUB/SUB short topic fast path 측정

## 후보

SUB receive에서 같은 짧은 topic을 연속 수신할 때 `MemorySegment.asSlice()`와
`mismatch()` 대신 원시 byte 비교로 기존 topic 문자열을 재사용하는 후보를 적용했다.
공개 interface는 변경하지 않았고, 긴 topic은 기존 경로를 유지했다.

## 1-client 분리 측정

- Core: release `0.10.1`
- 실행 순서: C 후 Java, 병렬 실행 없음
- 공통 조건: clients `1`, duration `2초`, runs `1`, I/O threads `4/4`, balanced auto-HWM,
  send/receive timeout `200ms`, connect-ready timeout `10000ms`
- C report: `/tmp/zlink-java-pubsub-one-client-c/multi/report/perf_c_multi_linux_20260813_043024_java-pubsub-one-client-c.txt`
- Java report: `/tmp/zlink-java-pubsub-one-client-java/multi/report/perf_java_multi_linux_20260813_043050_java-pubsub-one-client-java.txt`

1-client에서도 64B부터 4KiB까지 Java/C 비율은 `66.18 / 53.18 / 43.50 / 85.56%`였다.
100-client fan-out만의 문제가 아니라, SUB receive 한 건의 Java 경계 비용도 주요 원인이다.

## 후보 측정

- 공통 조건: clients `100`, duration `2초`, runs `1`, I/O threads `4/4`, balanced auto-HWM,
  send/receive timeout `200ms`, connect-ready timeout `10000ms`
- C report: `/tmp/zlink-java-pubsub-short-topic-c/multi/report/perf_c_multi_linux_20260813_043206_java-pubsub-short-topic-c.txt`
- Java report: `/tmp/zlink-java-pubsub-short-topic-java/multi/report/perf_java_multi_linux_20260813_043234_java-pubsub-short-topic-java.txt`

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 1,421,739 msg/s | 976,406 msg/s | 68.68% |
| 256B | 1,638,026 msg/s | 962,409 msg/s | 58.75% |
| 1024B | 1,408,608 msg/s | 582,351 msg/s | 41.34% |
| 4096B | 563,711 msg/s | 325,934 msg/s | 57.82% |
| 65536B | 101,999 msg/s | 69,197 msg/s | 67.84% |
| 131072B | 50,886 msg/s | 35,613 msg/s | 70.00% |

산술평균은 `60.74%`다. 공개 primitive-read 기준 `61.05%`보다 낮아 구현을 원복했다.
두 report는 모두 `status: complete`, result line `30/30`이다.

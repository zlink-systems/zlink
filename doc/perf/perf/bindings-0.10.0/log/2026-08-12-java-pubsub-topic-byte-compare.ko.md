# Java PUBSUB topic cache 비교 측정 결과

## 대상

`MULTI_PUBSUB / tcp`에서 반복 topic 비교를 `MemorySegment.mismatch()` 대신 Java byte loop로
수행하는 후보를 측정했다. public interface와 Core ABI는 변경하지 않았다.

Release Core `0.10.1`, clients `100`, duration `1초`, runs `1`, balanced auto-HWM에서
C를 먼저, Java를 다음에 단독 실행했다.

## 결과

| Message size | C throughput | Java throughput | C 대비 비율 |
|---|---:|---:|---:|
| 64B | 1,406,878 msg/s | 640,767 msg/s | 45.55% |
| 256B | 1,501,044 msg/s | 475,651 msg/s | 31.69% |
| 1024B | 1,033,779 msg/s | 390,603 msg/s | 37.78% |
| 4096B | 455,455 msg/s | 153,157 msg/s | 33.63% |
| 65536B | 101,629 msg/s | 48,560 msg/s | 47.78% |
| 131072B | 52,746 msg/s | 31,417 msg/s | 59.56% |
| 산술평균 | - | - | 42.67% |

최신 기준 `51.44%`보다 낮아 byte loop는 채택하지 않았다. `MemorySegment.mismatch()`가 짧은
반복 topic에서도 더 빠른 비교 경로다.

- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_153431_java-pubsub-topic-byte-compare-c.txt`
- Java report: `/home/hep7hep7/project/zlink/bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_153453_java-pubsub-topic-byte-compare-java.txt`

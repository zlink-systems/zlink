# Java PUBSUB single-thread metric collector 측정 결과

## 대상

Java Multi perf client가 하나의 application poll loop에서 metric을 기록하므로, `LongAdder`와
thread-local reservoir 대신 일반 counter를 사용하는 후보를 측정했다. public interface와
Core ABI는 변경하지 않았다.

Release Core `0.10.1`, clients `100`, duration `1초`, runs `1`, balanced auto-HWM에서
C를 먼저, Java를 다음에 단독 실행했다.

## 결과

| Message size | C throughput | Java throughput | C 대비 비율 |
|---|---:|---:|---:|
| 64B | 1,395,158 msg/s | 391,904 msg/s | 28.09% |
| 256B | 1,313,182 msg/s | 303,476 msg/s | 23.11% |
| 1024B | 1,326,151 msg/s | 269,635 msg/s | 20.33% |
| 4096B | 494,957 msg/s | 115,568 msg/s | 23.35% |
| 65536B | 95,484 msg/s | 47,837 msg/s | 50.10% |
| 131072B | 57,880 msg/s | 30,625 msg/s | 52.91% |
| 산술평균 | - | - | 32.98% |

최신 기준 `51.44%`보다 낮아 single-thread metric collector 후보는 채택하지 않았다.

- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_153827_java-pubsub-single-metric-collector-c.txt`
- Java report: `/home/hep7hep7/project/zlink/bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_153849_java-pubsub-single-metric-collector-java.txt`

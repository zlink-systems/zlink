# Java PUBSUB receive 후보 측정 결과

## 조건

Release Core `0.10.1`, TCP, clients `100`, duration `1초`, runs `1`, balanced
auto-HWM에서 각 후보마다 C를 먼저, Java를 다음에 단독 실행했다. Java binding 전체 test는
통과했다.

## private native receive bridge

`zlink_subscribe_part` 성공 뒤 필요한 frame size와 data address를 Java private native
bridge에서 함께 반환하는 후보다. Core ABI와 public interface는 변경하지 않았다.

| Message size | C throughput | Java throughput | C 대비 비율 |
|---|---:|---:|---:|
| 64B | 1,458,776 msg/s | 686,956 msg/s | 47.09% |
| 256B | 1,509,114 msg/s | 696,461 msg/s | 46.15% |
| 1024B | 1,384,514 msg/s | 594,408 msg/s | 42.93% |
| 4096B | 564,526 msg/s | 242,166 msg/s | 42.90% |
| 65536B | 107,001 msg/s | 47,423 msg/s | 44.32% |
| 131072B | 59,048 msg/s | 35,717 msg/s | 60.49% |
| 산술평균 | - | - | 47.31% |

최소 기준 70%와 최신 기준 51.44%에 모두 미달해 채택하지 않았다.

- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_160416_java-pubsub-native-recv-bridge-c.txt`
- Java report: `/home/hep7hep7/project/zlink/bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_160439_java-pubsub-native-recv-bridge-java.txt`

## receive failure wrapper pool

`DONT_WAIT` receive가 data 없이 끝날 때 외부로 반환되지 않는 Java wrapper를 internal pool로
되돌리는 후보다. public interface는 변경하지 않았다.

| Message size | C throughput | Java throughput | C 대비 비율 |
|---|---:|---:|---:|
| 64B | 1,384,647 msg/s | 676,741 msg/s | 48.87% |
| 256B | 1,370,073 msg/s | 646,560 msg/s | 47.19% |
| 1024B | 1,359,062 msg/s | 465,267 msg/s | 34.23% |
| 4096B | 495,669 msg/s | 217,521 msg/s | 43.88% |
| 65536B | 101,135 msg/s | 51,771 msg/s | 51.19% |
| 131072B | 52,361 msg/s | 34,319 msg/s | 65.54% |
| 산술평균 | - | - | 48.49% |

최소 기준 70%와 최신 기준 51.44%에 모두 미달해 채택하지 않았다.

- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_160720_java-pubsub-receive-failure-pool-c.txt`
- Java report: `/home/hep7hep7/project/zlink/bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_160745_java-pubsub-receive-failure-pool-java.txt`

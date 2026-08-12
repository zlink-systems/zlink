# Java socket-owned send invoker 측정 결과

## 대상과 변경

`PAIR`와 `DEALER` socket은 `send()`를 호출할 때마다 single-part와 multipart dispatch를 위한
capturing lambda를 새로 만들고 있었다. socket lifetime 동안 변하지 않는 dispatch 책임을 socket이
소유하도록 옮기고, `MessageOperations`에는 기존 public operation builder만 새로 만들도록 했다.

공개 `SendOperation`의 생성·제출·재사용 금지 규칙과 Message ownership은 변경하지 않았다.
Java 전체 테스트가 통과했다.

## PAIR / tcp 결과

Core release `0.10.1`, duration `1초`, runs `1`, balanced auto-HWM,
64·256·1024·65536·131072·262144B 조건에서 C를 먼저, Java를 다음에 단독 실행했다.

| Message size | C throughput | Java throughput | C 대비 비율 |
|---|---:|---:|---:|
| 64B | 2,505,931 msg/s | 1,608,375 msg/s | 64.18% |
| 256B | 1,218,553 msg/s | 1,170,000 msg/s | 96.02% |
| 1024B | 637,532 msg/s | 823,822 msg/s | 129.22% |
| 65536B | 39,229 msg/s | 63,576 msg/s | 162.06% |
| 131072B | 24,663 msg/s | 35,237 msg/s | 142.88% |
| 262144B | 14,835 msg/s | 15,576 msg/s | 105.00% |
| 산술평균 | - | - | 116.56% |

이전 같은 TCP pattern의 47.57%보다 68.99%p 높다. 이 값은 새 C와 Java report의 paired 결과이며,
전체 평균은 같은 방식으로 최신 transport·pattern 표본을 정규화한 뒤 계산한다.

- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260812_092424_java-send-invoker-pair-c.txt`
- Java report: `/home/hep7hep7/project/zlink/bindings/java/perf/results/single/report/perf_java_single_linux_20260812_092436_java-send-invoker-pair.txt`

## DEALER_DEALER / tcp 결과

같은 조건에서 C를 먼저, Java를 다음에 단독 실행했다.

| Message size | C throughput | Java throughput | C 대비 비율 |
|---|---:|---:|---:|
| 64B | 2,574,252 msg/s | 1,277,875 msg/s | 49.64% |
| 256B | 1,202,879 msg/s | 1,205,136 msg/s | 100.19% |
| 1024B | 705,458 msg/s | 820,022 msg/s | 116.24% |
| 65536B | 40,220 msg/s | 37,004 msg/s | 92.00% |
| 131072B | 24,952 msg/s | 23,954 msg/s | 96.00% |
| 262144B | 15,248 msg/s | 13,963 msg/s | 91.57% |
| 산술평균 | - | - | 90.94% |

이전 같은 TCP pattern의 44.14%보다 46.80%p 높다.

- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260812_092525_java-send-invoker-dealer-c.txt`
- Java report: `/home/hep7hep7/project/zlink/bindings/java/perf/results/single/report/perf_java_single_linux_20260812_092537_java-send-invoker-dealer.txt`

## PUBSUB / tcp 결과

`PUBSUB`은 topic-bound invoker 하나가 single-part와 multipart publish dispatch를 함께
수행하도록 했다. 같은 조건에서 C를 먼저, Java를 다음에 단독 실행했다.

| Message size | C throughput | Java throughput | C 대비 비율 |
|---|---:|---:|---:|
| 64B | 1,431,194 msg/s | 854,994 msg/s | 59.74% |
| 256B | 954,101 msg/s | 861,191 msg/s | 90.26% |
| 1024B | 579,275 msg/s | 723,105 msg/s | 124.83% |
| 65536B | 38,983 msg/s | 61,889 msg/s | 158.76% |
| 131072B | 24,569 msg/s | 33,975 msg/s | 138.28% |
| 262144B | 15,143 msg/s | 16,081 msg/s | 106.19% |
| 산술평균 | - | - | 113.01% |

이전 같은 TCP pattern의 38.02%보다 74.99%p 높다.

- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260812_092745_java-topic-invoker-pubsub-c.txt`
- Java report: `/home/hep7hep7/project/zlink/bindings/java/perf/results/single/report/perf_java_single_linux_20260812_092803_java-topic-invoker-pubsub.txt`

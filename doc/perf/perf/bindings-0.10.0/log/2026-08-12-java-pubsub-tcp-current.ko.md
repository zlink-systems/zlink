# Java TCP MULTI_PUBSUB 현재 측정

## 조건

- Core: release `0.10.1`
- Java: JDK `22.0.2+9`
- 순서: C 단독 실행 뒤 Java 단독 실행
- clients: `100`
- duration: `1초`
- runs: `1`
- HWM: balanced auto-HWM

## 결과

| Size | C Kmsg/s | Java Kmsg/s | Java/C |
|---|---:|---:|---:|
| 64B | 1473.449 | 537.636 | 36.49% |
| 256B | 1484.527 | 579.162 | 39.01% |
| 1024B | 882.126 | 455.858 | 51.68% |
| 4096B | 427.321 | 232.526 | 54.41% |
| 65536B | 99.603 | 67.959 | 68.23% |
| 131072B | 57.259 | 33.694 | 58.84% |
| 산술 평균 | - | - | **51.44%** |

수신 경로는 반복 topic decode cache와 subscriber별 caller-provided `TopicMessage` 재사용을 적용했다.
Java perf poller는 이미 채운 `PollEvents`를 다시 복사하지 않고 직접 읽는다.
이 값은 Java simple one-way 중앙값 목표 60%에 미달한다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_150415_java-pubsub-tcp-direct-pollevents-c.txt`
- Java report: `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_150438_java-pubsub-tcp-direct-pollevents-java.txt`

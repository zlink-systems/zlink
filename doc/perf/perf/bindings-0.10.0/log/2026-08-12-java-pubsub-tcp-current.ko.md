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
| 64B | 1499.454 | 691.628 | 46.13% |
| 256B | 1554.100 | 611.956 | 39.38% |
| 1024B | 1281.026 | 608.785 | 47.52% |
| 4096B | 521.569 | 223.901 | 42.93% |
| 65536B | 112.137 | 61.364 | 54.72% |
| 131072B | 53.086 | 38.434 | 72.40% |
| 산술 평균 | - | - | **50.51%** |

수신 경로는 반복 topic decode cache와 subscriber별 caller-provided `TopicMessage` 재사용을 적용했다.
이 값은 Java simple one-way 중앙값 목표 60%에 미달한다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_150029_java-pubsub-tcp-received-slot-c.txt`
- Java report: `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_150051_java-pubsub-tcp-received-slot-java.txt`

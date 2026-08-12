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
| 64B | 1487.995 | 598.525 | 40.22% |
| 256B | 1420.394 | 739.939 | 52.10% |
| 1024B | 1126.460 | 558.944 | 49.62% |
| 4096B | 588.660 | 242.222 | 41.15% |
| 65536B | 111.554 | 62.768 | 56.27% |
| 131072B | 59.450 | 39.140 | 65.83% |
| 산술 평균 | - | - | **50.86%** |

이 값은 Java simple one-way 중앙값 목표 60%에 미달한다. 다음 단계는 PUBSUB send·receive hot path의
allocation과 callback 경계 후보를 A/B로 확인하는 것이다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_144552_java-pubsub-tcp-current-c.txt`
- Java report: `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_144649_java-pubsub-tcp-current-java.txt`

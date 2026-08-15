# Java TCP MULTI_DEALER_ROUTER 현재 측정

## 조건

- Core: release `0.10.1`
- Java: JDK `22.0.2+9`
- 순서: C 단독 실행 뒤 Java 단독 실행
- clients: `100`
- duration: `1초`
- runs: `1`
- HWM: balanced auto-HWM

## 결과

| Size | C Kops/s | Java Kops/s | Java/C |
|---|---:|---:|---:|
| 64B | 176.210 | 92.695 | 52.60% |
| 256B | 179.689 | 93.373 | 51.96% |
| 1024B | 126.925 | 73.770 | 58.12% |
| 4096B | 161.645 | 84.513 | 52.28% |
| 65536B | 36.790 | 33.646 | 91.45% |
| 131072B | 20.986 | 17.017 | 81.09% |
| 산술 평균 | - | - | **64.58%** |

이 값은 Java routed one-way 중앙값 목표에 미달한다.

- C report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_150826_java-dealer-router-tcp-current-c.txt`
- Java report: `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_150848_java-dealer-router-tcp-current-java.txt`

## public API metric 재측정

이전 수치는 public contract 밖의 metric access가 있던 시점의 결과이므로 완료 근거로
사용하지 않는다. 현재 public primitive message read만 쓰는 Java perf에서 release Core
`0.10.1`, tcp, 100 clients, duration 2초, auto-HWM `balanced`, I/O thread 4,
`64/256/1024/4096/65536/131072B`를 C 다음 Java 순서로 한 번씩 실행했다.

| Size | C throughput (ops/s) | Java throughput (ops/s) | C 대비 |
|---:|---:|---:|---:|
| 64 | 187,209.0 | 128,524.5 | 68.65% |
| 256 | 179,906.0 | 125,004.0 | 69.48% |
| 1,024 | 173,366.0 | 120,338.5 | 69.41% |
| 4,096 | 161,448.0 | 119,068.5 | 73.75% |
| 65,536 | 35,504.0 | 50,182.5 | 141.34% |
| 131,072 | 20,854.5 | 21,681.5 | 103.96% |
| 산술평균 | - | - | **87.77%** |

Java/C 평균은 routed one-way 목표 `85%`를 통과한다.

- C: `/tmp/zlink-java-dr-sendsend-c-2s/multi/report/perf_c_multi_linux_20260813_042109_java-dr-sendsend-c-2s.txt`
- Java: `/tmp/zlink-java-dr-sendsend-java-2s/multi/report/perf_java_multi_linux_20260813_042135_java-dr-sendsend-java-2s.txt`

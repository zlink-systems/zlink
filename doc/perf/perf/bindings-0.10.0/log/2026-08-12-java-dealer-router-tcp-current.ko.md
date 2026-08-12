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

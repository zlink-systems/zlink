# Java STREAM native frame direct 측정 결과

## 대상

`MULTI_STREAM / tcp`에서 server callback이 header와 body를 Java `byte[]`에 합친 뒤
새 `Message`로 복사하던 경로를, `Message.allocate()`와 `Message.copyFrom()`으로 최종
native frame에 직접 기록하는 경로로 바꿨다. 공개 interface와 Core ABI는 변경하지 않았다.

Release Core `0.10.1`, clients `100`, duration `1초`, runs `1`, balanced auto-HWM에서
C를 먼저, Java를 다음에 단독 실행했다.

## 결과

| Message size | C throughput | Java throughput | C 대비 비율 |
|---|---:|---:|---:|
| 64B | 332,061 ops/s | 264,708 ops/s | 79.72% |
| 256B | 326,760 ops/s | 219,855 ops/s | 67.28% |
| 1024B | 311,088 ops/s | 213,602 ops/s | 68.66% |
| 65536B | 56,249 ops/s | 56,753 ops/s | 100.90% |
| 산술평균 | - | - | 79.14% |

이전 `54.31%`보다 `24.83%p` 높으며 Java simple one-way 최소 기준 `70%`를 통과한다.

- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_153020_java-stream-native-frame-direct-c.txt`
- Java report: `/home/hep7hep7/project/zlink/bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_153044_java-stream-native-frame-direct-java.txt`

## public API metric 재측정

public primitive message read만 사용하는 현재 Java perf에서 release Core `0.10.1`, tcp,
100 clients, duration 2초, auto-HWM `balanced`, I/O thread 4,
`64/256/1024/65536B`를 C 다음 Java 순서로 한 번씩 실행했다.

| Size | C throughput (ops/s) | Java throughput (ops/s) | C 대비 |
|---:|---:|---:|---:|
| 64 | 337,908.0 | 267,438.0 | 79.14% |
| 256 | 324,171.5 | 256,812.5 | 79.22% |
| 1,024 | 310,917.0 | 244,875.0 | 78.76% |
| 65,536 | 55,860.0 | 59,850.0 | 107.14% |
| 산술평균 | - | - | **86.07%** |

Java/C 평균은 multi routed echo 목표 `70%`를 통과한다.

- C: `/tmp/zlink-java-stream-c-2s/multi/report/perf_c_multi_linux_20260813_042225_java-stream-c-2s.txt`
- Java: `/tmp/zlink-java-stream-java-2s/multi/report/perf_java_multi_linux_20260813_042246_java-stream-java-2s.txt`

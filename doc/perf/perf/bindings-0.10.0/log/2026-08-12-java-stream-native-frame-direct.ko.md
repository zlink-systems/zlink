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

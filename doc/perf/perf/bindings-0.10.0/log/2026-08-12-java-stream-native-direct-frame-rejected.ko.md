# Java MULTI_STREAM native 직접 frame 조립 결과

## 대상과 조건

- 대상: `tcp / MULTI_STREAM`
- 조건: C를 먼저 단독 실행한 뒤 Java를 단독 실행했다. `duration=1`, `runs=1`, `clients=100`, balanced auto-HWM을 사용했다.
- Core: release `0.10.1`

## 변경

`PerfMultiStream`에서 `header`와 `body`를 Java `byte[]`에 모은 뒤 `Message.from`으로 복사하던 경로를, 출력 `Message`의 native payload에 직접 기록하도록 바꿨다. 공개 interface와 Core API는 변경하지 않았다.

## 결과

| Size | C Kops/s | Java Kops/s | Java/C |
|------|---------:|------------:|-------:|
| 64B | 328.353 | 173.155 | 52.74% |
| 256B | 332.736 | 89.758 | 26.97% |
| 1024B | 323.969 | 131.472 | 40.58% |
| 65536B | 53.721 | 40.298 | 75.01% |

산술 평균은 **48.82%**다. 기존 최종 값 54.31%보다 낮으므로 변경을 되돌렸다.

- C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_133059_java-stream-direct-frame-c.txt`
- Java: `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_133125_java-stream-direct-frame.txt`


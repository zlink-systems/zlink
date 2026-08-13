# Java multi socket request/reply C parity

`MULTI_DEALER_ROUTER_REQREP`의 Java client loop를 C
`perf_multi_socket_reqrep.hpp`와 비교했다. 모든 request submit이 backpressure일 때
Java가 poll 없이 다시 submit하던 경로를 C처럼 50ms completion poll로 변경했다.

Java는 `PERF_MULTI_REQREP_TIMEOUT_MS`를 읽지 않고 receive timeout을 request timeout으로
사용했다. C와 동일하게 이 환경 변수를 우선 사용하고, 미지정이면 200ms 기본 timeout을
사용한다. tail completion drain은 C의 `max(1000ms, request timeout * 4)` 규칙을 사용한다.
request callback의 non-OK completion은 C처럼 waiting 상태만 해제하며 runner 실패로
바꾸지 않는다.

## 검증

Java 22와 release Core `0.10.1`에서 다음 build/test를 통과했다.

```text
./gradlew --no-daemon :perf-multi:compileJava :perf-multi:test
```

같은 순서로 C 다음 Java를 각각 한 번만 실행했다. 조건은 tcp, client 100,
duration 1초, auto-HWM, message size 64/256/1024/4096/65536/131072 byte다.

| 구현 | 결과 파일 |
|---|---|
| C | `/tmp/zlink-java-reqrep-parity2-c/multi/report/perf_c_multi_linux_20260812_223942.txt` |
| Java | `/tmp/zlink-java-reqrep-parity2-java/multi/report/perf_java_multi_linux_20260812_223957.txt` |

| Size | C throughput | Java throughput | Java/C |
|---:|---:|---:|---:|
| 64B | 104243 | 44858 | 43.03% |
| 256B | 104586 | 41814 | 39.98% |
| 1024B | 92505 | 38347 | 41.45% |
| 4096B | 86587 | 42769 | 49.39% |
| 65536B | 24752 | 21411 | 86.50% |
| 131072B | 15771 | 10643 | 67.49% |
| 산술평균 | - | - | 54.64% |

최소 기준 50%는 통과했지만 중앙 목표 70%에는 도달하지 못했다.

## public API metric 재측정

이전 수치는 Java perf가 public contract 밖의 metric access를 사용하던 시점의 결과이므로
완료 근거로 사용하지 않는다. public primitive message read만 사용하는 현재 metric 경로에서
release Core `0.10.1`, tcp, client 100, duration 2초, auto-HWM `balanced`, I/O thread 4,
`64/256/1024/4096/65536/131072B` 조건으로 C 다음 Java를 한 번씩 실행했다.

| Size | C throughput (ops/s) | Java throughput (ops/s) | C 대비 |
|---:|---:|---:|---:|
| 64 | 105,746.5 | 60,377.5 | 57.10% |
| 256 | 104,385.5 | 58,789.5 | 56.32% |
| 1,024 | 96,692.5 | 56,424.5 | 58.36% |
| 4,096 | 90,134.0 | 55,207.0 | 61.25% |
| 65,536 | 23,695.5 | 32,008.0 | 135.08% |
| 131,072 | 16,056.0 | 18,943.5 | 117.98% |
| 산술평균 | - | - | **81.01%** |

Java/C 평균은 목표 `70%`를 통과한다.

- C: `/tmp/zlink-java-dr-reqrep-c-2s/multi/report/perf_c_multi_linux_20260813_041836_java-dr-reqrep-c-2s.txt`
- Java: `/tmp/zlink-java-dr-reqrep-java-2s/multi/report/perf_java_multi_linux_20260813_041904_java-dr-reqrep-java-2s.txt`

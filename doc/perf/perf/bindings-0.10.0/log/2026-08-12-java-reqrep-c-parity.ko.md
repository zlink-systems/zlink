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

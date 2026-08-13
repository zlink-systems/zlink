# Java PUB/SUB SUB routing-id 생략 후보 결과

## 후보와 결과

`SUB` 수신에서는 source routing-id가 없다는 C benchmark의 message shape를 이용해,
Java가 매 frame마다 native out slot을 읽고 null을 확인하는 작업을 생략하는 후보를
측정했다. public interface와 Core ABI는 변경하지 않았다.

동일 조건의 C와 Java를 순서대로 실행한 결과 평균 비율은 `56.99%`였다. 현재 완료
근거 `63.57%`보다 낮으므로 후보를 원복했다.

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 1,463,217.5 | 665,375.5 | 45.47% |
| 256B | 1,175,213.5 | 713,073.0 | 60.68% |
| 1024B | 976,936.5 | 397,603.0 | 40.70% |
| 4096B | 432,729.0 | 214,089.5 | 49.47% |
| 65536B | 80,464.5 | 55,025.5 | 68.38% |
| 131072B | 45,396.5 | 35,067.5 | 77.25% |
| 산술평균 | - | - | 56.99% |

- Core: release `0.10.1`
- 대상: `MULTI_PUBSUB / tcp`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 Java 실행, 병렬 실행 없음
- C report: `/tmp/zlink-java-sub-rid-c/multi/report/perf_c_multi_linux_20260813_214135_java-sub-rid-c.txt`
- Java report: `/tmp/zlink-java-sub-rid-java/multi/report/perf_java_multi_linux_20260813_214154_java-sub-rid-java.txt`

subscription contract test는 후보 적용 상태에서 통과했다. 성능 개선이 없으므로
구현은 남기지 않았다.

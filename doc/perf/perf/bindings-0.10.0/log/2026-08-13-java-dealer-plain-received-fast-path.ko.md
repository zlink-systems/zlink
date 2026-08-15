# Java DEALER/DEALER plain Received fast path 결과

## 변경

caller-provided `Received`가 이전 수신에서도 routing id, request metadata, multipart
cursor 없이 single part만 소유한 경우에는 새 frame만 교체한다. 이전 frame은 즉시
owner 경로로 닫아 thread-local message wrapper와 native slot 재사용 규칙을 유지한다.

다른 socket shape에서 사용했던 `Received`는 routing과 request 상태를 지워야 하므로
기존 일반 경로를 사용한다. public interface와 Core ABI는 변경하지 않았다.

## 측정

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 2,345,045.0 | 1,218,633.5 | 51.97% |
| 256B | 1,278,163.5 | 882,784.0 | 69.07% |
| 1024B | 668,466.5 | 634,243.5 | 94.88% |
| 4096B | 320,443.0 | 271,584.5 | 84.75% |
| 65536B | 68,888.0 | 72,788.0 | 105.66% |
| 131072B | 38,736.5 | 33,462.5 | 86.39% |
| 산술평균 | - | - | 82.12% |

- Core: release `0.10.1`
- 대상: `MULTI_DEALER_DEALER / tcp`, clients `100`, duration `2초`, runs `1`, balanced auto-HWM
- 실행: C 종료 후 Java 실행, 병렬 실행 없음
- C report: `/tmp/zlink-java-received-plain-c/multi/report/perf_c_multi_linux_20260813_214503_java-received-plain-c.txt`
- Java report: `/tmp/zlink-java-received-plain-java/multi/report/perf_java_multi_linux_20260813_214525_java-received-plain-java.txt`

`SocketPollingContractTest`는 변경 후 다시 실행해 통과했다.

# Java PUB/SUB TopicMessage bridge cache 측정

## 변경

`TopicPlane`의 caller-provided `TopicMessage` single-part receive 경로에서
`TopicMessageAccess`를 socket instance에 cache했다. 기존에는 한 frame마다
candidate message 준비와 결과 adopt에 각각 volatile bridge lookup이 있었다.
이 변경은 contract-owned 객체를 runtime이 채우는 책임을 유지하면서, hot path의
반복 lookup을 제거한다. public interface와 receive 결과는 변경하지 않는다.

## 측정

- Core: release `0.10.1`
- 실행 순서: C 후 Java, 병렬 실행 없음
- 공통 조건: `MULTI_PUBSUB`, tcp, clients `100`, duration `2초`, runs `1`,
  I/O threads `4/4`, balanced auto-HWM, send/receive timeout `200ms`,
  connect-ready timeout `10000ms`
- C report: `/tmp/zlink-java-pubsub-topic-access-c/multi/report/perf_c_multi_linux_20260813_043541_java-pubsub-topic-access-c.txt`
- Java report: `/tmp/zlink-java-pubsub-topic-access-java/multi/report/perf_java_multi_linux_20260813_043608_java-pubsub-topic-access-java.txt`

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 1,517,063 msg/s | 849,209 msg/s | 55.98% |
| 256B | 1,594,315 msg/s | 908,698 msg/s | 57.00% |
| 1024B | 1,395,663 msg/s | 623,452 msg/s | 44.67% |
| 4096B | 575,776 msg/s | 304,491 msg/s | 52.88% |
| 65536B | 114,177 msg/s | 71,036 msg/s | 62.22% |
| 131072B | 56,954 msg/s | 38,374 msg/s | 67.38% |

산술평균은 `56.69%`다. 이 1회 측정에서 성능 향상은 확인하지 못했다. 그러나
bridge 조회 책임을 socket 수명으로 한정하는 구조 개선이며 `SocketSubscriptionContractTest`,
`SocketPollingContractTest`를 통과했으므로 유지한다. 두 report는 모두
`status: complete`, result line `30/30`이다.

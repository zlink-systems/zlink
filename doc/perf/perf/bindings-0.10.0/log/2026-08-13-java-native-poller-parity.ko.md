# Java multi native poller parity 측정

## 변경

Java multi runner의 `PerfSocketPollSet`은 public `PollEvents` 객체를 채운 뒤 ready
socket을 찾았다. C 기준 runner는 native poller event의 user-data와 event mask를 읽은 뒤
해당 socket을 `DONT_WAIT`로 drain한다. Java runner도 동일하게 native event buffer에서
필요한 두 필드만 읽도록 변경했다.

이 변경은 Java binding의 public interface를 바꾸지 않는다. C와 Java가 같은 poller
의미와 receive 순서를 사용하도록 perf harness의 비용 경계를 맞춘다.

## 검증

- `:perf-multi:compileJava` 통과
- `SocketPollingContractTest` 통과

## 측정

- Core: release `0.10.1`
- 실행 순서: 각 대상에서 C 후 Java, 병렬 실행 없음
- 공통 조건: tcp, clients `100`, duration `2초`, runs `1`, I/O threads `4/4`,
  balanced auto-HWM, send/receive timeout `200ms`, connect-ready timeout `10000ms`

### MULTI_PUBSUB

- C report: `/tmp/zlink-java-pubsub-native-poller-c/multi/report/perf_c_multi_linux_20260813_045918_java-pubsub-native-poller-c.txt`
- Java report: `/tmp/zlink-java-pubsub-native-poller-java/multi/report/perf_java_multi_linux_20260813_045944_java-pubsub-native-poller-java.txt`

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 1,535,944 msg/s | 985,128 msg/s | 64.14% |
| 256B | 1,639,339 msg/s | 1,029,209 msg/s | 62.78% |
| 1024B | 1,446,808 msg/s | 973,866 msg/s | 67.31% |
| 4096B | 557,838 msg/s | 295,089 msg/s | 52.90% |
| 65536B | 115,420 msg/s | 77,213 msg/s | 66.90% |
| 131072B | 60,865 msg/s | 41,040 msg/s | 67.42% |

산술평균은 `63.57%`다.

### MULTI_DEALER_DEALER

- C report: `/tmp/zlink-java-dd-native-poller-c/multi/report/perf_c_multi_linux_20260813_050022_java-dd-native-poller-c.txt`
- Java report: `/tmp/zlink-java-dd-native-poller-java/multi/report/perf_java_multi_linux_20260813_050051_java-dd-native-poller-java.txt`

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 2,476,270 msg/s | 1,562,657 msg/s | 63.11% |
| 256B | 1,402,540 msg/s | 1,113,352 msg/s | 79.38% |
| 1024B | 893,785 msg/s | 822,998 msg/s | 92.08% |
| 4096B | 411,339 msg/s | 233,200 msg/s | 56.69% |
| 65536B | 110,853 msg/s | 59,044 msg/s | 53.26% |
| 131072B | 46,948 msg/s | 35,528 msg/s | 75.68% |

산술평균은 `70.03%`다. 두 report는 모두 `status: complete`, result line `30/30`이다.

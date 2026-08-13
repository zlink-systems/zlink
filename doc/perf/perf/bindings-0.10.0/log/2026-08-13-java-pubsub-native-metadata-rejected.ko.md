# Java PUB/SUB native metadata bridge 측정

## 후보

Java binding의 SUB single-part receive에서 `zlink_subscribe_part()`가 성공하면
frame size와 data address를 같은 native bridge 호출에서 반환하는 후보다. 기존의
`zlink_msg_size`와 `zlink_msg_data` critical FFM 호출 두 번을 없애는 목적이었다.

## 측정

- Core: release `0.10.1`
- 실행 순서: C 후 Java, 병렬 실행 없음
- 공통 조건: clients `100`, duration `1초`, runs `1`, I/O threads `4/4`, balanced auto-HWM,
  send/receive timeout `200ms`, connect-ready timeout `10000ms`
- message size: `64 / 256 / 1024 / 4096 / 65536 / 131072B`
- C report: `/tmp/zlink-java-bridge-metadata-c/multi/report/perf_c_multi_linux_20260813_034139_java-bridge-metadata-c.txt`
- Java report: `/tmp/zlink-java-bridge-metadata-java/multi/report/perf_java_multi_linux_20260813_034201_java-bridge-metadata-java.txt`

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 1,413,612 msg/s | 596,068 msg/s | 42.17% |
| 256B | 1,482,081 msg/s | 491,300 msg/s | 33.15% |
| 1024B | 1,345,032 msg/s | 332,141 msg/s | 24.69% |
| 4096B | 537,037 msg/s | 112,607 msg/s | 20.97% |
| 65536B | 104,961 msg/s | 56,153 msg/s | 53.50% |
| 131072B | 55,597 msg/s | 33,333 msg/s | 59.96% |

산술평균은 `39.07%`다. 공개 primitive-read 경로의 `61.05%`보다 낮으므로 native
metadata bridge 구현은 제거했다. 두 report는 모두 `status: complete`, result line
`30/30`이다.

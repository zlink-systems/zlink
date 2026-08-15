# Java send wrapper pool 후보 측정

## 후보

`Message.allocate()`와 `Message.from(...)`이 만든 wrapper를 성공 send 뒤
thread-local pool로 되돌리는 후보를 구현했다. send가 성공하면 public contract상
해당 `Message`는 더 이상 사용할 수 없으므로 ownership 규칙은 유지한다.

## 측정

- Core: release `0.10.1`
- 실행 순서: C 후 Java, 병렬 실행 없음
- 공통 조건: `MULTI_PUBSUB`, tcp, clients `100`, duration `2초`, runs `1`,
  I/O threads `4/4`, balanced auto-HWM, send/receive timeout `200ms`,
  connect-ready timeout `10000ms`
- C report: `/tmp/zlink-java-send-wrapper-c/multi/report/perf_c_multi_linux_20260813_050559_java-send-wrapper-c.txt`
- Java report: `/tmp/zlink-java-send-wrapper-java/multi/report/perf_java_multi_linux_20260813_050628_java-send-wrapper-java.txt`

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 1,397,341 msg/s | 541,722 msg/s | 38.77% |
| 256B | 1,518,784 msg/s | 530,960 msg/s | 34.96% |
| 1024B | 1,371,313 msg/s | 470,948 msg/s | 34.34% |
| 4096B | 528,066 msg/s | 257,096 msg/s | 48.69% |
| 65536B | 102,933 msg/s | 53,546 msg/s | 52.02% |
| 131072B | 45,492 msg/s | 25,247 msg/s | 55.50% |

산술평균은 `44.05%`다. 새 wrapper를 heap에 보관하는 경로가 기존의 짧은 수명 객체보다
불리하게 동작했다. 이 후보는 성능 개선이 없고 message 수명 경로를 복잡하게 하므로 원복했다.
두 report는 모두 `status: complete`, result line `30/30`이다.

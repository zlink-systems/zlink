# Java Dealer/Dealer pre-connect auto-HWM 후보 측정

## 후보

Java `MULTI_DEALER_DEALER` client에서 `recalculateAutoHwm()`을 connect 전에 한 번 더
호출하고, connect 뒤 기존 재계산을 유지했다. C helper의 pipe 생성 순서와 비교하기 위한
후보였다.

## 측정

- Core: release `0.10.1`
- 실행 순서: C 후 Java, 병렬 실행 없음
- 공통 조건: `MULTI_DEALER_DEALER`, tcp, clients `100`, duration `2초`, runs `1`,
  I/O threads `4/4`, balanced auto-HWM, send/receive timeout `200ms`,
  connect-ready timeout `10000ms`
- C report: `/tmp/zlink-java-dd-preconnect-hwm-c/multi/report/perf_c_multi_linux_20260813_050907_java-dd-preconnect-hwm-c.txt`
- Java report: `/tmp/zlink-java-dd-preconnect-hwm-java/multi/report/perf_java_multi_linux_20260813_050938_java-dd-preconnect-hwm-java.txt`

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 2,481,889 msg/s | 1,283,419 msg/s | 51.71% |
| 256B | 1,353,740 msg/s | 992,190 msg/s | 73.29% |
| 1024B | 918,251 msg/s | 652,519 msg/s | 71.06% |
| 4096B | 352,971 msg/s | 168,550 msg/s | 47.75% |
| 65536B | 103,996 msg/s | 55,747 msg/s | 53.60% |
| 131072B | 45,026 msg/s | 35,943 msg/s | 79.83% |

산술평균은 `62.87%`다. 현재 context와 socket option 설정이 이미 connect 전 기준을
적용하므로 추가 재계산은 성능 개선이 아니었다. 후보는 원복했다. 두 report는 모두
`status: complete`, result line `30/30`이다.

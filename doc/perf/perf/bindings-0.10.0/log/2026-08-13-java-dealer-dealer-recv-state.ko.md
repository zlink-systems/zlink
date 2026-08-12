# Java TCP DEALER/DEALER receive state 결과

release Core `0.10.1`을 사용했고 C와 Java를 직렬로 실행했다. 조건은 `tcp`,
`MULTI_DEALER_DEALER`, message size `64,256,1024,4096,65536,131072`, duration 5초,
client 100, auto-HWM `balanced`다.

| Size | C throughput (msg/s) | Java throughput (msg/s) | C 대비 |
|---:|---:|---:|---:|
| 64 | 2,595,050.2 | 1,795,525.0 | 69.19% |
| 256 | 1,437,571.2 | 1,221,825.6 | 84.99% |
| 1,024 | 966,369.6 | 808,788.0 | 83.70% |
| 4,096 | 432,040.6 | 375,358.6 | 86.88% |
| 65,536 | 96,528.6 | 80,801.8 | 83.71% |
| 131,072 | 50,766.2 | 43,733.4 | 86.15% |
| 산술평균 | - | - | **82.44%** |

single-part `DONT_WAIT` 수신에서 이미 조회한 multipart state를 다시 `ThreadLocal`에서
가져오지 않도록 변경했다. 이전 확인값 81.32%보다 1.12%p 상승했지만 목표 90%에는 미달한다.

- C: `/tmp/zlink-java-recv-state-c/multi/report/perf_c_multi_linux_20260813_020354_java-recv-state-c.txt`
- Java: `/tmp/zlink-java-recv-state-java/multi/report/perf_java_multi_linux_20260813_020435_java-recv-state-java.txt`

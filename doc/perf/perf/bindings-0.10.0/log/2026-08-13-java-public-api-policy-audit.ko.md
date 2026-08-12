# Java perf public API 정책 점검

## 판정

기존 Java metric header 측정 경로는 `ContractAccess`를 직접 호출했다. 이는
`doc/perf/PERF_POLICY.md`의 Java perf가 binding public API만 사용해야 한다는
규칙에 맞지 않는다. 해당 호출을 `Message.dataBuffer()` 공개 API로 교체했다.

이 경로는 모든 Java multi 수신 metric에 사용됐으므로, 아래 PUB/SUB 재측정값 외의
기존 Java 결과는 완료 근거로 사용하지 않고 재측정한다.

## MULTI_PUBSUB / tcp 측정

- Core: release `0.10.1`
- 실행 순서: C 후 Java, 병렬 실행 없음
- 공통 조건: clients `100`, duration `1초`, runs `1`, I/O threads `4/4`, balanced auto-HWM,
  send/receive timeout `200ms`, connect-ready timeout `10000ms`
- message size: `64 / 256 / 1024 / 4096 / 65536 / 131072B`
- C report: `/tmp/zlink-java-policy-public-c/multi/report/perf_c_multi_linux_20260813_032729_java-policy-public-c.txt`
- Java report: `/tmp/zlink-java-policy-public-java/multi/report/perf_java_multi_linux_20260813_032751_java-policy-public-java.txt`

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 1,581,382 msg/s | 576,432 msg/s | 36.45% |
| 256B | 1,508,336 msg/s | 642,542 msg/s | 42.60% |
| 1024B | 1,124,109 msg/s | 505,082 msg/s | 44.93% |
| 4096B | 526,153 msg/s | 140,816 msg/s | 26.76% |
| 65536B | 110,436 msg/s | 71,553 msg/s | 64.79% |
| 131072B | 55,973 msg/s | 43,472 msg/s | 77.67% |

산술평균은 `48.87%`이며 Java 단순 one-way 목표 `90%`에 미달한다. 두 report는
모두 `status: complete`, result line `30/30`이다.

## 검증

- `bindings/java/perf/`에서 `ContractAccess`, runtime/native FFI import가 없는 것을 확인했다.
- Java 22와 release Core `0.10.1`으로 `:perf-multi:compileJava` 및
  `RawSocketIntegrationTest`를 실행했고 성공했다.

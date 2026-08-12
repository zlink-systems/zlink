# Java perf public API 정책 점검

## 판정

기존 Java metric header 측정 경로는 `ContractAccess`를 직접 호출했다. 이는
`doc/perf/PERF_POLICY.md`의 Java perf가 binding public API만 사용해야 한다는
규칙에 맞지 않는다. 해당 호출을 `Message.readIntLe()`, `Message.readByte()`,
`Message.readLongLe()` 공개 API로 교체했다.

이 경로는 모든 Java multi 수신 metric에 사용됐으므로, 아래 PUB/SUB 재측정값 외의
기존 Java 결과는 완료 근거로 사용하지 않고 재측정한다.

## MULTI_PUBSUB / tcp 측정

- Core: release `0.10.1`
- 실행 순서: C 후 Java, 병렬 실행 없음
- 공통 조건: clients `100`, duration `1초`, runs `1`, I/O threads `4/4`, balanced auto-HWM,
  send/receive timeout `200ms`, connect-ready timeout `10000ms`
- message size: `64 / 256 / 1024 / 4096 / 65536 / 131072B`
- C report: `/tmp/zlink-java-public-read-c/multi/report/perf_c_multi_linux_20260813_033328_java-public-read-c.txt`
- Java report: `/tmp/zlink-java-public-read-java/multi/report/perf_java_multi_linux_20260813_033349_java-public-read-java.txt`

| Size | C throughput | Java throughput | Java / C |
|---:|---:|---:|---:|
| 64B | 1,552,496 msg/s | 688,052 msg/s | 44.32% |
| 256B | 1,586,369 msg/s | 672,195 msg/s | 42.37% |
| 1024B | 1,328,523 msg/s | 640,326 msg/s | 48.20% |
| 4096B | 424,713 msg/s | 240,318 msg/s | 56.58% |
| 65536B | 97,353 msg/s | 71,298 msg/s | 73.24% |
| 131072B | 46,707 msg/s | 47,447 msg/s | 101.58% |

산술평균은 `61.05%`이며 Java 단순 one-way 목표 `90%`에 미달한다. 두 report는
모두 `status: complete`, result line `30/30`이다.

## 검증

- `bindings/java/perf/`에서 `ContractAccess`, runtime/native FFI import가 없는 것을 확인했다.
- Java 22와 release Core `0.10.1`으로 `:perf-multi:compileJava` 및
  `RawSocketIntegrationTest`를 실행했고 성공했다.

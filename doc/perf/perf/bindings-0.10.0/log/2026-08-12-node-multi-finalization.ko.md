# 2026-08-12 Node Multi finalization

## 측정 조건

- Core runtime: release `0.10.1` (`~/.cache/zlink/core/0.10.1/linux-x64`)
- 순서: 같은 transport·pattern에서 C를 먼저 실행하고 binding을 실행했다.
- 병렬 실행: 하지 않았다. 한 process가 종료된 뒤 다음 process를 시작했다.
- 공통: clients `100`, duration `1초`, runs `1`, I/O threads `4`, auto-HWM `balanced`,
  transport/pattern transition `0ms`.
- size: 일반 Multi는 64·256·1024·4096·65536·131072B, STREAM은
  64·256·1024·65536B다.

## Node 변경과 검토

`SendSocket`은 socket 수명에 고정된 submitter를 재사용한다. STREAM도 같은 routed submitter
구조를 사용하며, 수신 reply는 이미 받은 routing-id bytes를 직접 전달한다. `SendOperationBase`는
payload 상태를 직접 소유해 별도 `OperationPayload` 객체를 만들지 않고 scalar `Message`의 성공한
submit에서 배열을 만들지 않는다. public interface와 send consume, DONTWAIT backpressure 의미는
변경하지 않았다.

Sol 검토는 다음을 확인했다.

- socket-lifetime submitter 구조는 contract-safe다.
- operation 객체 pooling, native handle cache, Buffer zero-copy send, batch/prefetch는 ownership 또는
  synchronous HWM 결과를 바꾸므로 채택하지 않는다.
- payload 상태 통합은 실제 public send 경로의 객체 수를 줄이는 POSDDD 정리로 채택할 수 있다.

64B `MULTI_DEALER_DEALER/tcp`에서 socket-lifetime submitter 적용 뒤 378.6K에서 423.7K msg/s로
상승했다. payload 상태 통합은 433.6K msg/s를 기록했지만 모든 size에서 일관된 상승은 아니었다.
구조 단순화 효과로만 유지했다.

## 최종 report

- C: `bindings/c/perf/results/multi/report/*_final2-<transport>-<pattern>-c.txt`
- Node: `bindings/node/perf/results/multi/report/*_final2-<transport>-<pattern>-{small,mid,large}.txt`
- Java: `bindings/java/perf/results/multi/report/*_final2-java22-tcp-<pattern>.txt`
- .NET: `bindings/dotnet/perf/results/multi/report/*_final2-dotnet-tcp-<pattern>.txt`

Java 재측정의 첫 시도는 system Java 21이 Java 22 class file을 실행하지 못해
`server_ready_timeout`으로 끝났다. 최종 측정은 저장된 Java 22 runtime
`~/.cache/jdks/jdk-22.0.2+9`를 명시해 다시 실행했으며 모든 report가 complete다.

Node 결과 비율과 최종 판정은 계획서의 `9.4.2 Multi suite`, Java/.NET TCP 결과는 `11.6`에 기록한다.

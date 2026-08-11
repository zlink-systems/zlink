# Message wrapper pool 작업 log

## 적용 범위

- .NET은 기존 `[ThreadStatic]` routed receive wrapper pool을 중복 구현하지 않고 계약 주석과 재사용 test를 보강했다.
- Java는 ROUTER receive wrapper를 `ThreadLocal` bounded pool에서 재사용한다.
- Java `Received`는 part reference를 먼저 제거한 뒤 owner 전용 cleanup으로 wrapper를 반환한다.
- Java single-part routed send는 기존 public builder를 유지하면서 multipart list와 vector를 만들지 않고 native single-frame send를 사용한다.

## 검토와 제외 후보

Sol 리뷰는 public `Message.close()`가 owner `Received`의 alias를 남긴 채 wrapper를 반환하는 초기 구현을 NO-GO로 판정했다. Public close는 wrapper를 반환하지 않고 native resource만 정리하도록 수정했다. Owner `Received`가 내부 reference를 제거한 뒤 호출하는 cleanup만 pool 반환을 허용한다.

DEALER caller-provided receive와 no-data receive 실패 wrapper까지 pool 범위를 넓힌 후보는 `RequestReplyTerminationProbe`에서 `ctxTerm` 종료 회귀가 발생했다. 후보를 제거한 뒤 해당 종료 test와 전체 Java test가 통과했다.

최종 Sol 재검토는 stale external reference의 반복 `close()`가 재사용된 wrapper의 새 frame을 닫을 수 있어 기존 정식 idempotent close 정책과 충돌한다고 NO-GO로 판정했다. 같은 Java 객체 identity를 재사용하는 한 generation별로 구분할 수 없으므로, 사용자 승인에 따라 반환 후 reference 사용 금지를 정식 공통 정책과 Node·Java·.NET exact interface에 반영했다. Single-part 직접 send에서 빠져 있던 `IllegalStateException`의 `TERMINATED` 변환도 multipart 경로와 같게 수정했다.

정식 정책 반영 뒤 최종 Sol 재검토는 GO다. Owner cleanup이 reference를 제거한 뒤에만 wrapper를 반환하고, public close와 receive 실패는 pool에서 제외하며, bounded thread-local pool의 초기화·overflow·cross-thread 처리가 계약과 일치함을 확인했다. Public API signature는 변경하지 않았다.

## 검증

- .NET Release test: `149 passed, 0 failed, 0 skipped`.
- Java test: `75 tests, 0 failures, 0 errors, 0 skipped`.
- Java 적용 전 대비 최종 throughput 산술평균 개선: `22.994%`.
- Java C 대비 throughput ratio 산술평균: `71.435%`, 평균 latency ratio: `1.517x`.
- 모든 C, .NET, Java perf는 병렬 실행하지 않고 종료를 확인한 뒤 다음 process를 실행했다.

## Java routed echo 추가 개선

Wrapper pool 적용 뒤 single-part routed echo에서 매 receive마다 `RoutingId` 객체와 capturing
sender lambda를 만드는 비용을 제거했다. `Received`는 raw routing-id bytes와 socket별 고정
single/multipart sender를 내부에서 보관하며, multipart 또는 request/reply fallback만 기존
`RoutingId` 경로를 사용한다. Public API signature와 send ownership/error mapping은 유지했다.

Retained 후보는 C 대비 size별 `61.876% / 58.567% / 61.442% / 57.064% / 96.148% /
93.513%`, 산술평균 `71.435%`를 기록했다. Wrapper pool 적용 결과 대비 산술평균은
`104.803%`다.

자체 두 번째 후보로 DEALER의 매 호출 lambda를 socket별 고정 method reference로 바꿨다.
75개 test는 통과했지만 retained 후보 대비 throughput 산술평균이 `90.885%`여서 제거했다.

Sol 리뷰는 DONT_WAIT receive 실패와 후속 `zlink_errno()`를 binding native bridge 한 번으로
합치는 후보를 제안했다. Public API와 Core ABI를 바꾸지 않고 구현했으며 75개 test가 통과했다.
C 대비 throughput 산술평균은 `69.915%`, retained 후보 대비 `97.787%`여서 제거했다.

두 번째 자체 후보와 Sol 후보 모두 실제 public receive/send 경로에서 검증했지만 성능이
하락했다. 추가 저위험 hot path 후보가 없어 routed one-way 목표 `85%` 미달 상태를
측정값 기준 `보류`로 확정했다.

- paired C: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260811_172800_java-raw-rid-c.txt`
- retained Java: `/home/hep7hep7/project/zlink/bindings/java/perf/results/multi/report/perf_java_multi_linux_20260811_172826_java-raw-rid-senders.txt`
- rejected own candidate: `/home/hep7hep7/project/zlink/bindings/java/perf/results/multi/report/perf_java_multi_linux_20260811_173428_java-stable-dealer-invokers.txt`
- rejected Sol candidate: `/home/hep7hep7/project/zlink/bindings/java/perf/results/multi/report/perf_java_multi_linux_20260811_173640_java-recv-errno-bridge.txt`

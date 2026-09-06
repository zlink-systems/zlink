# Framework gRPC 비교 bench 5언어 캠페인 — 결정 기록

계획: [`../framework-bench-with-grpc-5lang-plan.ko.md`](../framework-bench-with-grpc-5lang-plan.ko.md)

각 항목은 결정, 근거, 적용 위치를 남긴다. 번복하면 새 ID로 기록하고 옛 항목에 번복 표시를 단다.

## FB-001 — raw binding 행을 ROUTER↔ROUTER로 통일한다

- **결정** (2026-09-06, 사용자): ZLink raw binding 행의 소켓을 DEALER→ROUTER에서 ROUTER↔ROUTER로
  바꾼다. `bindings/c/bench/with_grpc`의 `zlink-c` 기준값도 같은 구성으로 맞춘다.
- **근거**: framework 행은 RouteMesh ROUTER↔ROUTER인데 raw 행이 DEALER→ROUTER면
  `zlink-framework-<lang> / zlink-<lang>` 비율에 framework 계층 비용과 소켓 패턴 비용이 함께
  들어간다. 규격 §7의 0.80 판정이 framework 계층만 재려면 두 행이 같은 패턴이어야 한다.
- **확인한 코드**: framework 서버 `framework/languages/dotnet/bench/with-grpc/ZLinkServer/Program.cs:20-24`
  가 `AddRouteMesh` + `AddRequestHandler` + `AddSendHandler`. raw client
  `framework/languages/dotnet/bench/with-grpc/Client/Program.cs:493,512,530`이 DEALER.
  raw server `ZLinkRawServer/Program.cs:25-28`은 이미 ROUTER.
  C 기준 bench는 `bindings/c/bench/with_grpc/zlink/bench_zlink_client.cpp:530-531`이 DEALER,
  `bench_zlink_server.cpp:273-274`가 ROUTER.
- **적용**: Phase 0(job `fwb-02`).

## FB-002 — send 비교는 gRPC unary `Command` → `Empty`를 유지한다

- **결정** (2026-09-06, 사용자): `send-saturation`의 gRPC 대응물을 현행 unary로 둔다.
  client-streaming 셀을 추가하지 않으며 proto에 RPC를 더하지 않는다.
- **경위**: 감독관이 "gRPC는 왕복, ZLink는 단방향이라 비대칭"이라는 우려를 제기하고
  client-streaming 추가를 제안했다. 사용자가 이 bench의 목적은 **서비스 측면 비교**이며,
  같은 업무를 각 스택으로 구현했을 때의 비용을 재는 것이라고 확정했다.
- **근거**: 실제 서비스에서 응답이 필요 없는 호출은 unary + `Empty`로 구현한다.
  client-streaming은 업로드·대량 적재용이며 단발 명령에 쓰지 않는다. gRPC에 단방향 호출
  원시 기능이 없어 왕복을 치르는 것은 gRPC의 특성이고, 서비스 측면 비교에서는 그 비용이
  결과에 그대로 드러나는 것이 맞다.
- **서술 제약**: 이 셀을 전송 속도 차이로 쓰지 않는다. "응답이 필요 없는 명령을 처리할 때
  gRPC는 unary 왕복을 치르고 ZLink는 단방향 send로 끝난다. 이 조건에서 차이는 N배였다"가
  정확한 문장이다.
- **적용**: Phase 1(job `fwb-01`) 규격 문서, Phase 6 보고서 서술.

## FB-003 — 판정 패턴과 기준식은 기존 규격을 따른다

- **결정** (2026-09-06): 판정 기준 패턴은 `request-window`, 기준식은 규격 §7의 두 식
  (`zlink-<lang> / zlink-c >= 0.80`, `zlink-framework-<lang> / zlink-<lang> >= 0.80`)을 그대로 쓴다.
- **근거**: `request-window`에서 gRPC unary `Echo`와 ZLink request는 서버 처리 확인이라는
  같은 보장을 주므로 정면 비교가 성립한다. 기존 규격을 이 캠페인이 바꾸지 않는다.

## job 기록

| job | Phase | 모델 | 상태 | 결과 |
|---|---|---|---|---|
| `fwb-01` | 1(문서) | opus | 진행 중 | 규격 문서 5언어 중립화, FB-001~003 반영 |

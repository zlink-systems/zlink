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

## FB-004 — 두 ZLink 행의 endpoint 개수 차이는 그대로 둔다

- **결정** (2026-09-06, 감독관): raw binding 행은 request echo endpoint와 command endpoint를
  분리하고 framework 행은 RouteMesh 연결 하나가 request 처리기와 send 처리기를 함께 가진다.
  이 차이를 맞추지 않는다.
- **경위**: job `fwb-01`이 FB-001로도 해소되지 않는 비대칭으로 보고했다.
- **근거**: 패턴을 한 번에 하나씩 측정하므로 `send-saturation` 셀에서는 두 행 모두 send만
  전달하고 request 셀에서는 request만 전달한다. 따라서 어느 셀도 오염되지 않는다.
  raw 쪽 분리는 두 패턴이 같은 socket을 공유할 때를 대비한 구성이다.
- **제약**: 한 셀에서 두 패턴을 동시에 실행하는 시나리오가 생기면 이 전제가 깨진다.
  그때 구성을 다시 결정한다. 규격에 이 전제를 명시한다.
- **적용**: job `fwb-01` 후속 지시.

## FB-005 — 0.80 판정은 두 payload 크기 모두에서 만족해야 한다

- **결정** (2026-09-06, 감독관): 규격 §7.2의 두 판정식은 payload 크기별로 적용한다.
  `1024`와 `4096` **둘 다** 만족해야 그 언어가 통과다. 한 크기만 만족하면 통과가 아니다.
- **경위**: job `fwb-01`이 기존 규격에 payload 한정자가 없다고 보고했다.
- **근거**: 1024에서 유지되다가 4096에서 무너지는 구성은 실제 문제다. 보고서가 어차피 두
  크기를 모두 싣기 때문에 크기별 판정은 추가 비용이 없고 정보를 감추지 않는다.
- **적용**: job `fwb-01` 후속 지시, Phase 6 보고서 주 표.

## FB-006 — .NET with-grpc bench는 커밋된 상태로 빌드되지 않았다 (기존 결함)

- **관찰** (job `fwb-02`, 감독관 검증 완료): `ZLinkRawServer/Program.cs`가
  `received.RequestSeq`를 사용하는데 `bindings/dotnet`의 `Received`에는 그 멤버가 없다.
  현재 계약은 `Received.ReplyToken`이다(`bindings/dotnet/src/Zlink/Contracts/Messaging/Received.cs:58`).
  `RequestSeq`는 커밋 `bf2da527dc`에서 사라졌다.
- **영향**: `run_local.sh:19`가 이 프로젝트를 빌드하고 스크립트는 `set -euo pipefail`이므로
  **runner 전체가 빌드 단계에서 실패한다.** 즉 .NET with-grpc bench는 커밋된 상태로 한 번도
  실행되지 않았다. `log/` 디렉터리가 비어 있던 것과 일치한다.
- **처리**: `received.ReplyToken is not null`로 수정했다. binding 계약 변경에 bench가 따라오지
  않은 경우이며 계약 쪽 문제가 아니다.
- **후속**: bench가 어떤 정기 gate에도 걸려 있지 않아 이 파손이 드러나지 않았다. 5언어 확장
  뒤 bench를 어떤 주기로 돌릴지는 0.18.0 후보로 올린다.

## FB-007 — C 기준 bench의 server CPU·memory 값은 구조적으로 0이었다 (기존 결함)

- **관찰** (job `fwb-02`, 감독관 검증 완료): `bindings/c/bench/with_grpc/run_local.sh`가
  `setsid "${server}" & ; pid=$!`로 서버를 띄웠다. `setsid`는 호출자가 프로세스 그룹 리더가
  아니면 fork하므로 `$!`는 곧 사라지는 wrapper의 PID다.
- **영향**: client가 `SERVER_PID`로 서버 CPU·RSS를 표본화하는데 그 PID가 서버가 아니었다.
  따라서 C bench가 낸 server CPU·memory 열은 전부 무의미한 값이었다. cleanup이 그 그룹을
  죽여도 서버에 닿지 않는 문제도 함께 있었다.
- **처리**: 각 서버가 자기 PID를 pidfile에 기록하고 runner가 그 값을 읽도록 바꿨다.
- **주의**: 과거 C bench 결과의 server CPU·memory 수치를 이 캠페인의 비교에 인용하지 않는다.

## 범위 밖으로 확인하고 미룬 항목

| 항목 | 처리 |
|---|---|
| 규격 §7.4의 단위 불일치 (C report는 KOPS, `RESULT` 라인은 초당 완료 수) | Phase 1 공용 집계기가 정규화한다. 집계기 도입 뒤 §7.4를 삭제한다 |
| C report 표의 열 구성이 규격 §4와 다름 (`Submitted`·`Completed`·`Errors` 등 추가 열) | Phase 1 공용 집계기가 흡수한다 |
| `bindings/c/bench/BENCH_POLICY.md`와 FB-001의 관계 — `with_grpc`의 DEALER 셀을 없앨지 병행할지 | Phase 0(job `fwb-02`)이 정책 문서를 읽고 판단해 보고한다 |

## job 기록

| job | Phase | 모델 | 상태 | 결과 |
|---|---|---|---|---|
| `fwb-02` | 0 | opus | 코드 변경 완료, 측정 진행 중 | raw ROUTER↔ROUTER 전환(.NET·C), 기존 결함 2건(FB-006·FB-007) 수정 |
| `fwb-01` | 1(문서) | opus | 완료·커밋 `146db4da4c` | 규격 5언어 중립화(ko 338행·en 359행), FB-001~003 반영. 고정값·RPC 미변경 확인. FB-004·FB-005 추가 지시 |

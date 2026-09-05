# .NET Canonical 잔여 실패 수정 결과

2026-09-05. 두 대상 테스트를 B(기존 fixture 결함)로 수정했다. **최종 Canonical 3회는 모두 16/16**, Canonical을 제외한 **전체 unit gate는 1939/1939**다. 별도 Stateful 실행의 간헐 실패 1건과 두 sample runner의 종료 실패는 아래 BLOCKERS에 남긴다.

## 변경 파일

- `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/CanonicalActorJoinIngressReplyTests.cs`
- [수정 전 진단](./stage12-dotnet-canonical-residual-diagnosis.md)
- 이 결과 문서

Framework runtime·Core·binding·spec·다른 언어는 수정하지 않았다. 시작 branch는 `main`이며 commit하지 않았다. 시작 시 존재한 Node provenance, SampleConfigurationPolicyRegressionTests 변경과 untracked 디렉터리, 작업 도중 다른 작업자가 생성한 Core 보고서는 보존했다.

## HANDOVER 테스트

`CanonicalActorJoinRequest_HandoverLeavesReplyRouteToCore`의 원인은 이전 raw peer의 Hello에 대한 **추가 Admit DATA를 replacement가 읽지 않은 것**이다. `ReplyJoin`은 새 ingress의 correlation 57과 native reply token을 사용했다. replacement pair가 추가로 supersede된 증거는 없었고, 앞선 DATA가 Core FIFO에서 reply를 막았다.

| 순서 | 수정 후 동작과 근거 |
|---|---|
| 1 | old peer의 correlation 56 ingress에 Backpressured를 주입한다. |
| 2 | replacement pair가 HANDOVER되고 첫 Admit을 소비한다. 기존 logical admission assertion을 유지한다. |
| 3 | old peer가 추가 Hello를 보낸다. 기존 trace에서 target은 추가 Admit DATA를 replacement pair로 보낸다. |
| 4 | test `:622`에서 기존 `ReceiveAsync(runtime.Source)`로 추가 Admit을 소비한다. |
| 5 | old peer를 닫고 simulated Core terminal 뒤 pending reply 재제출이 멈추는 기존 assertion을 검증한다. |
| 6 | replacement correlation 57의 `ReplyJoin(Accepted)`와 caller decode가 성공한다. 최종 3회 test 전체 시간은 722/725/744 ms이며 기존 request timeout 2초를 유지한다. |

공개 `Systems.Zlink` API만 사용한 `scratchpad/stage12-dotnet-canonical/public-repro/`에서도 같은 현상을 분리했다. ROUTER HANDOVER 뒤 old Hello의 DATA response는 replacement로 전송된다. replacement의 새 request ingress에 보관된 `Reply()`를 제출했을 때 다음 결과를 얻었다.

| 공개 API 대조 | 결과 |
|---|---|
| 추가 DATA 미수신 | 69.9 ms에 Reply Submit=Ok, 2068.6 ms에 caller `TimedOut(101)` |
| 추가 DATA 수신 | 66.8 ms에 Reply Submit=Ok, 67.0 ms에 `prior-admit` 수신, 68.6 ms에 caller Ok와 correlation 57 payload |

증거는 `public-repro-withheld.log`, `public-repro-drain.log`다. 이 repro의 old socket dispose 결과인 `Terminated(103)`를 HANDOVER `NOT_CONNECTED` 검증으로 계산하지 않았다.

- 소유 계층: Core가 pair 선택·opaque reply token·DATA/REPLY FIFO, raw caller fixture가 DATA 수신을 소유한다.
- Spec 조항: Core socket README §4 HANDOVER(`:164-171`), §6 Request와 reply의 FIFO(`:1078-1080`).
- 교차언어 대조: C++ `raw_mesh_node_owner.cpp:3669`도 수신한 reply token을 전달한다. .NET runtime의 reply re-pin이나 별도 poller가 필요한 구조적 차이는 없다.
- 변경 분류: **B — raw fixture의 DATA 수신 누락**.

## Lost-reply 테스트

`ManagedSource_Command28Request_Retries_Same_Correlation_After_Lost_Reply`는 첫 reply를 보내지 않은 상태에서 **source→target 연결을 추가하여 첫 attempt의 방향을 HANDOVER로 대체**한다(`test :249`). source RID가 target RID보다 작으므로 Core가 이 outbound 방향을 선택한다. Runtime에 timeout 분할·재시도 상태를 추가하지 않았다.

| 순서 | 수정 후 동작과 assertion |
|---|---|
| 1 | target→source 연결에서 원래 8초 operation을 제출한다. 첫 ingress correlation이 `operationId.Low`임을 확인한다. |
| 2 | 기존 target owner인 `ZLinkActorHandoffAdmissions.AdmitAsync`로 application decision을 실행하고 Rejected semantic reply를 보관한다. 첫 native reply는 fixture가 보류한다. |
| 3 | source→target 연결의 reciprocal HANDOVER가 첫 native attempt를 `NotConnected(109)`로 끝낸다. 기존 diagnostics의 `family-order-trace.log`에서 해당 native exception과 이후 같은 durable operation의 완료를 관찰했다. |
| 4 | 원래 7초 ingress 대기 안에서 retry를 받는다. 첫 ingress와 OperationId/correlation이 같아야 한다. |
| 5 | retry도 같은 `ZLinkActorHandoffAdmissions`에 전달한다. **동일 semantic reply 객체, application decision 실행 횟수 1**을 assert한다(`:253-257`). 별도 fixture cache나 dedup 규칙은 추가하지 않았다. |
| 6 | 새 ingress의 native reply token으로 terminal을 제출한다. caller는 원래 OperationId, Ok, Rejected와 원래 JSON payload를 받는다. 실제 reply submit은 1회다. 최종 3회 test 전체 시간은 789/806/799 ms다. |

Target admission deadline도 submit 직전에 같은 8초 budget으로 고정한다(`:198`). 중복 실행 방지 검증에는 production dispatcher가 사용하는 admission owner(`ZLinkFrameworkRuntimeActors.cs:2518`, `ZLinkActorHandoffAdmissions.cs:35-58`)를 재사용했다. 이 테스트의 실행 횟수는 그 owner에 넘긴 application decision callback을 센다.

- 소유 계층: Core가 HANDOVER와 native completion, Framework sender가 durable operation replay, 기존 target admission owner가 decision 중복 실행 방지를 소유한다.
- Spec 조항: Core socket README §4·§6 completion 표(`:1149`); actor-model sender bullets(`:668-680`)의 남은 deadline 전부 사용, terminal envelope 없는 결과만 replay, admitted-no-reply의 DeadlineExceeded.
- 교차언어 대조: C++ `raw_mesh_node_owner.cpp:195-204`도 남은 deadline을 request에 전달하며 `:225-228`에서 route-unavailable 결과를 replay한다. 이번 변경은 .NET fixture에만 해당한다.
- 변경 분류: **B — 고정 5초 attempt 분할을 전제로 한 fixture**.

수정 전/후 규칙 수(실패를 만든 fixture의 추가 가정): **2 → 0**. 미수신 DATA 추월과 고정 attempt 분할 가정을 제거했다. Runtime의 규칙·상태·helper 수는 그대로이며, target decision 보관은 기존 admission owner 하나를 사용한다.

## 검증 결과

모든 .NET 실행은 지정 환경과 `/tmp/zlink-dotnet-gate.lock`을 사용했다. NuGet SHA-256은 `be4ab2bbff665e04886c139dbab712da71b3c7fdcef412ab6b795fa816ad5f3a`, native SHA-256은 `98f3499696009ee5d43a1680ab5423c306d28af7592c1ca48fb40f3ee20773eb`다. UnitTests output의 native도 같은 hash다. Hash별 cache는 `/dev/shm/zlink-tmp-dotnet/nuget-be4ab2bbff665e04`다. `--artifacts-path`, `ulimit -v`는 사용하지 않았다.

증거 root는 `scratchpad/stage12-dotnet-canonical/`다. Test 결과 수치는 TRX에서 확인했다.

| 검증 | 결과 | 증거 |
|---|---|---|
| 수정 전 대상 2건 | 0/2 | `baseline.log`, `baseline.trx` |
| 최소 수정 focused | 2/2 | `focused.log`, `focused.trx` |
| decision exactly-once 보강 후 Canonical 1회차 | **16/16**, 23초 | `canonical-final-1.{log,trx}` |
| Canonical 2회차 | **16/16**, 21초 | `canonical-final-2.{log,trx}` |
| Canonical 3회차 | **16/16**, 20초 | `canonical-final-3.{log,trx}` |
| StatefulServiceRuntimeTests 별도 1회 | **62/63**, 105 결과 1건 | `stateful.{log,trx}` |
| 전체 unit gate 1회, `FullyQualifiedName!~CanonicalActorJoinIngressReplyTests` | **1939/1939**, 6분 7초. Stateful 부분도 **63/63** | `full.{log,trx}` |
| TicTacToe 1회 | client `tictactoe=completed`, **runner exit 137** | `sample-TicTacToe.{log,exit}`, `sample-evidence/TicTacToe/` |
| SupportChat 1회 | client `supportchat=completed`, **runner exit 137** | `sample-SupportChat.{log,exit}`, `sample-evidence/SupportChat/` |
| whitespace와 변경 범위 | 통과 | `git diff --check`; test 파일과 요청된 보고서만 작업 변경 |

Canonical에는 `--blame-hang --blame-hang-timeout 5m`을 적용했다. Hang dump는 없다. 전체 gate가 제외한 Canonical fixture에만 마지막 decision 검증 보강이 있으므로 전체 gate는 반복하지 않았다.

## BLOCKERS

### Sample runner의 종료 실패

TicTacToe의 play-a/play-b/api-a/api-b, SupportChat의 support/api/session이 cleanup에서 SIGKILL되었다. 두 runner는 client 완료 뒤 `kill -INT`, 20×0.1초 대기, `kill -9`를 수행한다(`TicTacToe/run_sample.sh:24-43`, `SupportChat/run_sample.sh:26-45`). Role 로그에는 shutdown 진입 기록이 없다.

Core·binding을 참조하지 않는 **기본 Microsoft.Extensions.Hosting Host**를 동일하게 bash background에서 실행한 대조에서, SIGINT 뒤 2초가 지나도 프로세스가 유지되었고 SIGTERM 뒤 정상 종료(exit 0)했다. `host-signal-repro/{Repro.csproj,Program.cs,run.sh}`, `host-signal-repro.log`에 재현을 보존했다. 따라서 관찰한 runner 종료 실패를 Canonical reply 또는 Core close defect로 단정하지 않는다. 이 별도 runner 문제의 제안은 background Host에 전달하는 종료 signal을 SIGTERM으로 수정하는 것이다. 이번 두 fixture 수정에서는 runner와 종료 budget을 변경하지 않았다. **두 sample을 PASS로 계산하지 않는다.**

### 간헐 실패 기록

- 별도 Stateful 실행에서 `DurableSenderPreservesExhaustionCauseAndOriginalOperation(operationKind: 13, scenario: "ready-after-submit")`가 expected Ok(0), actual ProtocolError(105)였다(`DurableSenderRuntimeTests.cs:125`). 뒤의 전체 gate에서는 같은 Stateful 63건이 모두 통과했다. 해당 test/runtime을 수정하지 않았고 원인은 미확정이다.
- decision 검증 보강 전 Canonical 3회차에서 HANDOVER의 기존 protocol-error count assertion이 0 대신 1이었다(당시 `test :590`, 현재 `:624`). 원래 replacement timeout 실패와 다른 지점이다. 단독 HANDOVER trace 10/10, 최종 Canonical 48/48에서는 재현되지 않았다. 해당 assertion을 유지했다.
- 위 간헐 오류를 조사한 predecessor-order console 반복은 8회 prefix 통과 뒤, 수정하지 않은 `CanonicalActorJoinRequest_PendingReplyUsesTerminalDeadline`의 fake-time 만료 대기에서 중단됐다(`test :885`). `family-order-trace.log`에 보존했으며 이 진단 실행을 정식 Canonical gate 결과에 합산하지 않았다.

현재 확인된 Core/binding defect는 없다. 남은 검증 실패와 원인 미확정 항목을 숨기거나 assertion·deadline을 변경하지 않았다.

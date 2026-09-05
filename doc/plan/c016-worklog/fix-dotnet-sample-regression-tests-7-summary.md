# .NET SampleRegressionTests 실패 진단과 수정

감독이 assertion 변경의 근거와 검증 결과를 확인하기 위한 기록이다. 수정 범위는
`framework/languages/dotnet/tests/Zlink.Framework.SampleRegressionTests/`의 기존 test 파일이다.
Sample, runner, runtime, binding, Core와 보호 문서는 변경하지 않았다. Commit하지 않았다.

## 기준 커밋 대조

**요청의 “`4bad5ac979`에서 7개 모두 통과” 전제는 해당 커밋의 파일과 일치하지 않는다.**
수정 전 test assembly로 `4bad5ac979`의 sample·E2E·문서 파일을 읽게 실행하자 같은 assertion에서
7개 모두 실패했다. 대상 test method는 baseline과 조사 시점 HEAD 사이에 변경되지 않았다.
`DeliveryDispatchRegressionTests.cs`의 차이는 다른 deadline test의 추가뿐이다.

- `4bad5ac979`: 2026-08-29 17:55:14 +0900.
- `3cbfbde4f9`: 2026-08-26 11:11:16 +0900. Sample 증거를 공통 계약에 맞추고
  ShoppingMall의 운영 query endpoint를 추가했다. 아래 6개 assertion이 이를 반영하지 못했다.
- `3b54beae06`: 2026-08-25 22:40:51 +0900. Server spec을 주제별 디렉터리로 옮겼다.
  Actor destroy test가 삭제된 이전 경로를 읽고 있었다.
- `git log -p 4bad5ac979..HEAD -- <대상 sample·runner·spec 경로>`에는 위 원인 변경이 없다.
  `8e76335988`의 공통 종료 처리, `0046fa5797`의 SIGKILL 보고 변경은 재현된 assertion의 원인이 아니다.
  `redis-common.sh`의 종료 소유권과 기존 Docker 격리 검사를 그대로 유지했다.

Baseline 재현은 branch 전환 없이 `/tmp/zlink-seven-baseline-4bad5ac979`에 `git archive`를 풀고,
수정 전 `bin/Debug/net8.0`을 그 아래 `test-bin`에 복사해 `dotnet vstest --TestCaseFilter:<7 methods>`로
실행했다. Source 검사 대상은 archive이며, runtime 전체를 baseline에서 다시 빌드한 결과는 아니다.
이 test들은 assertion 대상의 텍스트와 파일 존재 여부를 검사한다.

## Test별 판정

아래 수정 위치의 기준 디렉터리는 `framework/languages/dotnet/tests/Zlink.Framework.SampleRegressionTests/`다.
분류 **b**는 의도적으로 바뀐 계약·검사 대상·문서 위치에 test가 적응하지 못한 경우다.
변경 분류 **A**는 기존 공통 계약에 대한 test 적응이며 Framework runtime 변경은 없다.

| Test | 최초 실패 assertion | 원인 커밋 | Drift / 수정 위치 | 소유 계층 / spec 조항 / parity / 분류 |
| --- | --- | --- | --- | --- |
| `ShoppingMall_Runner_Uses_Isolated_Docker_Redis_And_Redis_Stores` | shell에 `shoppingmall-server-evidence=completed` 포함 | `3cbfbde4f9` | **b**, `ShoppingMallRegressionTests.cs:93`. `/self-check/assert`의 성공 판정, CommerceApi evidence, 각 Workflow node의 주문 시작을 Bash·PowerShell 양쪽에서 검사한다. 뒤에 가려져 있던 PowerShell의 이전 helper·오류 문구 assertion도 같은 근거로 수정했다. | Sample runner가 server evidence를 관찰한다. `common/sample/event/shoppingmall.ko.md` §10.1. Node·Java도 self-check와 sample evidence를 사용한다. **A**. |
| `Sample_Health_Checks_Use_Location_Readiness` | 모든 host source에서 `IZLinkLocationRuntimeQuery` 금지 | `3cbfbde4f9` | **b**, `Regression.cs:318`. `/health` handler 집합에서 `IZLinkLocationReadiness`·`IsPeerReadyAsync` 사용이 존재하는지 검사하고, 그 집합에서 query 금지를 유지한다. 각 handler가 모두 readiness API를 호출해야 한다는 검사로 확대하지 않는다. ShoppingMall의 `/self-check/relocate`, `/self-check/owner`, `/self-check/mesh-ready`는 public 운영 query의 사용자이므로 파일 전체 금지는 잘못된 범위였다. | Framework public Location API와 sample HTTP endpoint. `spec/server/languages/dotnet/interfaces/08-location-maintenance.ko.md` §3, `05-location-relocation/01-location-runtime.ko.md` §1.2. 다른 언어는 HTTP health와 runner의 route readiness를 구분한다. **A**. |
| `DeliveryDispatch_Contracts_Match_Common_Role_Model` | `deliverydispatch courier-session: bound courier=courier-a` 포함 | `3cbfbde4f9` | **b**, `DeliveryDispatchRegressionTests.cs:236`. `CourierSessionBinder`의 공통 `deliverydispatch-courier bound` 발행과 각 courier의 server log 1회를 Bash·PowerShell에서 검사한다. Message·역할·Actor 소유권 검사는 유지한다. | CourierSession이 bind를 수행하고 runner가 증거를 관찰한다. `common/sample/deliverydispatch/README.ko.md` §4, §10.1. Node·Java도 같은 marker와 각 courier 1회를 검사한다. **A**. |
| `SupportChat_Runner_Uses_Isolated_Docker_Redis_And_Location_Store` | shell에 `supportchat-server-evidence=completed` 포함 | `3cbfbde4f9` | **b**, `SupportChatRegressionTests.cs:185`. Runner가 자체 출력하던 marker 대신 API·Support 로그의 created, agent-joined와 모든 상태 전이를 Bash·PowerShell에서 검사한다. Client completion·closed typing marker와 Redis 검사는 유지한다. 뒤에 가려진 ConversationSpot의 이전 상태 로그 assertion도 현재 emitter와 일치시켰다. | Client가 completion을 발행하고 서버가 lifecycle 증거를 발행한다. `common/sample/supportchat/README.ko.md` §10.1. Node·Java도 같은 상태 증거와 client marker를 확인한다. **A**. |
| `DotNet_Docs_Keep_Actor_Destroy_Entry_Owned` | `11-spot-model.ko.md` 읽기에서 `FileNotFoundException` | `3b54beae06` | **b**, `Regression.cs:463`. 경로를 `03-spot-actor/01-spot-model.ko.md`로 고쳤다. Destroy exact API, Entry Spot 소유권, disconnect 비파괴 의미와 금지 문구 검사는 모두 유지한다. | Entry Spot이 Actor destroy를 소유한다. `spec/server/03-spot-actor/01-spot-model.ko.md` §7 및 `common/sample/tictactoe/README.ko.md` §6.2·§7.4. Node·Java도 Entry 소유권 검사를 둔다. **A**. |
| `DeliveryDispatch_Runner_Uses_Isolated_Docker_Redis_And_Location_Store` | shell에 `topology=ready` 포함 | `3cbfbde4f9` | **b**, `DeliveryDispatchRegressionTests.cs:112`. 각 server route와 두 courier actor route의 readiness 1회를 검사한다. 뒤에 가려진 이전 runner completion 요구·server evidence 금지도 공통 spec의 client server-evidence 검증으로 교체했다. Redis·Location Store 검사는 유지한다. | Sample이 readiness·evidence를 발행하고 runner가 확인한다. `common/sample/deliverydispatch/README.ko.md` §10.1. Node·Java도 역할별 readiness와 server-evidence marker를 확인한다. **A**. |
| `TicTacToe_Runner_Verifies_Client_And_Server_Evidence` | shell에 `stream-inbound sample=TicTacToe` 포함 | `3cbfbde4f9` | **b**, `TicTacToeRegressionTests.cs:327`. 공통 client self-check 증거, reconnect한 player-x bind 1회, 두 player의 leave·destroy 각각 1회, observer destroy 0회를 양 runner에서 검사한다. Dispatch error 금지를 유지하고 `LeaveGameMsg`와 leave 증거의 연결은 실제 handler에서 검사한다. | Client self-check와 Play lifecycle이 증거를 발행한다. `common/sample/tictactoe/README.ko.md` §10.1. Node·Java도 같은 marker와 lifecycle 횟수를 확인한다. **A**. |

## 교차언어 근거

Node·Java는 source를 읽어 대조했으며 이 작업에서 해당 언어 suite나 sample을 실행하지 않았다.

- Node: `test/contract/sample-regression.test.js:2051`의 Entry destroy 검사,
  `:2124`의 runner 위임 검사, `:2149`의 공유 Redis 격리 검사. 각
  `samples/{ShoppingMall,SupportChat,DeliveryDispatch,TicTacToe}.Ts/Runner/sample-runner.mjs`가
  공통 server·client evidence를 검사한다. 현재 source는 위 불변식을 유지한다.
- Java: `zlink-framework-testkit/src/contractTest/java/systems/zlink/framework/testkit/SampleReleaseGateContractTest.java:171`
  의 실행별 Docker Redis 검사, `:872`의 Entry destroy 문서 검사.
  `samples/java/{ShoppingMall,SupportChat,DeliveryDispatch,TicTacToe}/run_sample.sh`도
  공통 evidence를 검사한다. 현재 source는 위 불변식을 유지한다.
- Health의 exact .NET API 이름을 검사하는 동일 test는 Node·Java에 없다. 예를 들어 Java ShoppingMall
  OrderWorkflow `Program.java:112`와 Node DeliveryDispatch `dispatch-api-server.ts:24`는 HTTP health를
  제공하고 runner는 별도 route readiness 증거를 확인한다. 언어별 endpoint 구조 차이이며
  .NET 운영 query를 host 파일 전체에서 금지할 근거가 아니다.

수정 전/후 규칙 수: sample·runner 실행 규칙 **변경 없음**, 공통 종료 처리 소유자 **1 → 1**.
Test가 요구하던 이전 증거 규칙과 공통 spec의 현행 증거 규칙 **2 → 1**. 신규 runtime 상태·helper·분기는 없다.

## 검증 결과

- 수정 전 HEAD: 대상 test **0/7**, 동일 7개 실패. `/tmp/zlink-dotnet-sample-seven-before.log`.
- `4bad5ac979` artifact 대조: 대상 test **0/7**, 같은 assertion 실패.
  `/tmp/zlink-dotnet-sample-seven-baseline.log`.
- 수정 후 focused test: **7/7 통과**, 실패·skip 0. `/tmp/zlink-dotnet-sample-seven-after.log`.
- 최종 전체 `Zlink.Framework.SampleRegressionTests`: **157/157 통과**, 실패·skip 0, test duration 5초.
  `/tmp/zlink-dotnet-sample-regression-full.log`. 전체 suite는 수정 완료 후 한 번 실행했다.
- Sample run: 해당 없음. Sample·runner 변경이 없어 조건부 실행 요구에 해당하지 않는다.
- `git diff --check`: 통과.
- 독립 문서 검토 완료. Health 검사 집합의 범위와 player-x bind 횟수 설명을 코드·spec 근거에 맞춰 보정했다.

사용자 지정 `TMPDIR`, `ZLINK_LIBRARY_PATH`, `UseSharedCompilation=false`,
`MSBUILDDISABLENODEREUSE=1`, `DOTNET_CLI_TELEMETRY_OPTOUT=1`, `.nupkg` SHA-256으로 분리한
`NUGET_PACKAGES`와 `flock -w7200 /tmp/zlink-dotnet-gate.lock`을 사용한다.

## BLOCKERS

실행·수정 blocker와 남은 test 실패는 없다. Baseline 통과 이력은 위 artifact 재현 결과와 달라
감독 기록의 대조가 필요하다. 그 불일치는 assertion 원인 진단이나 수정 자체를 막지 않는다.

# Stage 2 sample runner SIGKILL 판정 결과

## 결과

.NET과 Node sample runner는 정상 종료 유예 시간이 지난 뒤 role process에 SIGKILL을 보내야 하면
성공으로 판정하지 않는다. 실패 출력은 role 이름과 종료 상태를 포함하며, runner 성공 marker는
cleanup 판정이 성공한 뒤에만 출력한다. timeout과 retry 횟수는 변경하지 않았다.

수정 전에는 scenario 완료와 cleanup 강제 종료를 모두 성공 한 가지로 판정했다. 수정 후에는
scenario 성공과 cleanup 성공을 각각 만족해야만 runner 성공 한 가지가 된다.

## Runner별 변경

### .NET Linux

- `samples/redis-common.sh`: cleanup의 SIGINT를 받은 role의 이름을 stdout log 경로에서 보존한다.
  같은 role에 이어지는 SIGKILL 또는 wait status 137/-9와 cleanup 함수가 직접 실행한 SIGKILL을 aggregate
  runner 상태 파일에 한 번만 기록한다. cleanup 밖에서 시나리오가 의도적으로 실행하는 SIGKILL은 teardown
  실패로 오인하지 않는다.
- `samples/run_samples.sh`: 개별 runner 출력을 cleanup 판정까지 보류한다. 상태 파일에 강제 종료가
  있으면 role과 PID를 출력하고 exit 137로 종료하며 보류한 완료 marker는 출력하지 않는다.

### .NET PowerShell

- `samples/sample_runner.ps1`: `Start-SampleDotnetAssembly -Name`의 role 이름을 PID와 연결한다.
  유예 시간 뒤 `Kill($true)`가 필요하거나 process가 137/-9로 종료되면 모든 process 정리를 마친 뒤
  role 이름을 포함한 예외를 발생시킨다.

### Node

- `samples/run-sample.mjs`: child의 exit code와 signal을 보존한다. cleanup 진입 전에 확인된 137/-9와
  SIGKILL, 유예 시간 뒤 runner가 보낸 SIGKILL을 role별 teardown 실패로 합친다.
- scenario와 browser runner의 완료 출력 및 `PASS <sample>`을 cleanup 성공 뒤로 옮겼다. teardown
  실패 시 log dump를 통해 browser의 `PASS`가 다시 노출되지 않으며 run directory는 보존한다.

## 계약 테스트

- .NET focused `SampleRegressionTests`: 2 passed.
  - `SampleRunnersFailWhenRoleCleanupRequiresSigkill`
  - `IntegratedSampleRunnerIncludesEveryCommonSample`
- Node `test/contract/sample-regression.test.js`: 52 passed. 실제 sample self-check가 깨끗하게 종료하면
  기존 PASS 목록을 검사하고, 그렇지 않으면 role이 명시된 teardown 실패와 PASS 미출력을 검사한다.
- Node runner focused contract: 3 passed.
- PowerShell parser, `bash -n`, `node --check`, `git diff --check`: passed.

전체 .NET `Zlink.Framework.SampleRegressionTests`는 157개 중 150개가 통과하고 기존 범위의 7개가
실패했다. 이번에 추가한 종료 판정 테스트는 통과했다. 실패 목록은 BLOCKERS에 기록한다.

## Sample 실행 결과

지정된 lock과 환경을 사용해 TicTacToe와 Bingo를 언어별로 실행했다. 현재 package에서는 네 실행
모두 teardown 결함을 드러냈으며 runner가 이를 성공으로 숨기지 않았다.

- .NET TicTacToe: exit 137. `api-b`, `api-a`, `play-b`, `play-a`가 cleanup에서 SIGKILL되었다.
- .NET Bingo: exit 137. `session-b`, `session-a`, `api-b`, `api-a`, `matchmaking`, `play-a`,
  `play-b`가 cleanup에서 SIGKILL되었다.
- Node TicTacToe.Ts: non-zero. `play-b`가 cleanup에서 SIGKILL되었다. 최종 contract 실행은 이 실패에서
  PASS marker가 출력되지 않는 것도 확인했다.
- Node Bingo.Ts: exit 1. `matchmaking`, `play-b`가 cleanup에서 SIGKILL되었다. PASS 및 scenario 완료
  marker는 출력되지 않았다.

## BLOCKERS

- 현재 Core/package의 role shutdown이 유예 시간 안에 끝나지 않아 TicTacToe와 Bingo의 clean-teardown
  성공 결과를 얻을 수 없다. 이 harness 작업에서는 timeout 증가, retry 추가 또는 runtime/sample 수정으로
  완화하지 않았다.
- 전체 .NET sample regression에는 이번 변경과 무관한 7개 기존 실패가 있다:
  `ShoppingMall_Runner_Uses_Isolated_Docker_Redis_And_Redis_Stores`,
  `Sample_Health_Checks_Use_Location_Readiness`,
  `DeliveryDispatch_Contracts_Match_Common_Role_Model`,
  `SupportChat_Runner_Uses_Isolated_Docker_Redis_And_Location_Store`,
  `DotNet_Docs_Keep_Actor_Destroy_Entry_Owned`,
  `DeliveryDispatch_Runner_Uses_Isolated_Docker_Redis_And_Location_Store`,
  `TicTacToe_Runner_Verifies_Client_And_Server_Evidence`.

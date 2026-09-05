# .NET sample cleanup exit 137 수정 결과

2026-09-05. 감독 검토용 결과다. Linux Bash runner가 Generic Host에서 무시되는 SIGINT를
보내고, 정상 shutdown의 descriptor 전파 대기보다 먼저 SIGKILL을 보낸 것이 원인이다.
공통 runner에서 SIGTERM을 전달하고 shutdown 기본 deadline인 30초까지 종료를 관찰하도록
수정했다. TicTacToe와 Bingo는 각각 **3/3**, 역할별 `wait`는 **33/33 exit 0**이며 cleanup
SIGKILL은 없다. 최종 aggregate도 cleanup 대상 **31/31 exit 0**이지만, ZoneWorld ZW-G4의
기능 실패로 **sample 6/7, aggregate exit 1**이다. 요청된 전체 PASS에는 도달하지 못했다.
Framework runtime과 Core·binding은 수정하지 않았다.

## 증거와 소유 계층

전체 증거는 [`scratchpad/fix-dotnet-sample-teardown-137/`](../../../scratchpad/fix-dotnet-sample-teardown-137/)에
보존했다. 진단용 runner 복사본은 원본의 SIGINT 및 2초 대기 뒤 dump를 수집한다. 별도
SIGTERM 진단에서는 2초 시점의 heap dump와 정상 종료까지의 시간을 수집한다. 이 복사본
실행을 정식 sample PASS로 계산하지 않았다.

| 관찰 | 근거 | 판정 |
|---|---|---|
| SIGINT 뒤 TicTacToe 4개·Bingo 7개 역할이 계속 실행된다. | `diagnosis-{TicTacToe,Bingo}/*.status`, `processes.txt` | `SigIgn=0000000000001007`에 SIGINT가 포함되고 SIGTERM은 `SigCgt`에 포함된다. 역할들은 runner와 같은 process group의 Bash background 자식이다. |
| SIGINT 뒤 api-a/play-a는 Main의 Task 대기, dispatch 및 native poll 대기에 있다. | 각 `diagnosis-*/{api-a,play-a}.dmp`, `*.stack.txt` (`clrstack -all`) | Host shutdown 진입 로그가 없고 `Context.Dispose → zlink_ctx_term` 정지 스택도 없다. |
| 기본 Microsoft.Extensions.Hosting Host도 SIGINT 뒤 2초 동안 계속 실행되고 SIGTERM에는 exit 0이다. | `plain-host.log`; 기존 `scratchpad/stage12-dotnet-canonical/host-signal-repro/` 재실행 | Zlink를 참조하지 않는 대조에서도 signal 문제가 재현된다. |
| SIGTERM에는 즉시 Application shutdown과 Draining에 진입한다. | `term-{TicTacToe,Bingo}/evidence/*/logs/` | ApplicationStopping handler가 signal을 받지 못하는 단계는 해소된다. |
| SIGTERM 뒤 2초 시점은 descriptor 전파 대기다. | `term-*/{api-a,play-a}.dmp`, `*.stack.txt` (`clrstack -all`, `dumpasync`) | 아래 async stack으로 Framework의 기존 지연을 확인했다. |
| 같은 역할이 모두 exit 0으로 끝난다. | `term-TicTacToe/term-results.txt`: 6483–6486 ms; `term-Bingo/term-results.txt`: 6533–6540 ms | 이 재현은 socket/poller 미정리, durable sender 또는 ClientServer의 close 정지가 아니다. |

SIGTERM 이후 대표 async stack은 다음과 같다.

```text
Task.DelayPromiseWithCancellation
  ZLinkFrameworkDrainExecutor.WaitForDescriptorPropagationAsync
    ZLinkFrameworkDrainExecutor.ExecuteWithProgressAsync
      ZLinkDrainCoordinator.ExecuteSharedAsync

ZLinkDrainCoordinator.DrainAsync
  ZLinkFrameworkMaintenanceRuntime.ExecuteShutdownAsync
    ZLinkFrameworkHostRuntimeCoordinator.StopAsync
      Microsoft.Extensions.Hosting.Internal.Host.StopAsync
        HostingAbstractionsHostExtensions.WaitForShutdownAsync
```

`term-TicTacToe/play-a.stack.txt:370`, `api-a.stack.txt:252`,
`term-Bingo/play-a.stack.txt:382`, `api-a.stack.txt:342`에서 전파 대기를 확인했다.
기존 `ZLinkFrameworkDrainExecutor.cs:129-130,474-485,607-611`은 polling 1초 +
Store read timeout 5초 + scheduler jitter 0.1초를 기다린다. 기존 file log와
`ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1` trace에서 `descriptor_propagated`,
`accepted_drained`, `Stopped/None`까지 진행했다. 임시 runtime 로깅은 추가하지 않았다.

SIGINT 무시는 .NET native signal 등록이 inherited `SIG_IGN`을 존중하는 동작과 일치한다.
Generic Host의 ConsoleLifetime은 SIGTERM을 `StopApplication()`으로 전달한다.
근거: [.NET 8 signal 설치 코드](https://github.com/dotnet/runtime/blob/v8.0.0/src/native/libs/System.Native/pal_signal.c),
[ConsoleLifetime signal 처리](https://github.com/dotnet/runtime/blob/v8.0.0/src/libraries/Microsoft.Extensions.Hosting/src/Internal/ConsoleLifetime.netcoreapp.cs).

## 계약과 변경

- 소유 계층: **sample runner**가 자식 process에 종료 signal을 전달하고 exit를 관찰한다.
  Framework의 기존 `ZLinkFrameworkHostRuntimeCoordinator`가 shutdown/drain/resource 정리 순서를 소유한다.
- Spec 조항: **05-host-relocation-flow §14**(`:759-828`)의 admission seal·accepted work drain·순서 있는 자원 정리,
  **§17**(`:899`)의 `Shutdown` 기본 deadline **30초**.
- 교차언어 대조: C++ `samples/TicTacToe/run_sample.sh:189-207`은 SIGTERM 후 300×0.1초,
  Java `samples/runner-common.sh:190-224`는 SIGTERM 후 기본 900×0.1초를 관찰한다.
  C++ GameQuest `run_sample.sh:343-352`는 의도적인 owner SIGKILL의 exit 137을 확인하고 PID를 제거한다.
  이번 .NET 차이는 runner signal과 관찰 시간이며 하위 transport의 구조적 차이가 아니다.
- 변경 분류: **B — 기존 runner 결함**. Runtime 계약·재시도·종료 순서는 변경하지 않았다.
- 수정 전/후 규칙 수: **cleanup 종료 정책 2개 → 1개, 종료 순서 구현 7곳 → 1곳**.
  SIGINT 후 강제 종료하는 정책과 ZoneWorld 즉시 강제 종료 정책을 기존 공통 runner 모듈로 모았다.
  신규 timer·retry·runtime state·signal 변환 adapter는 없다.

수정 전 원인은 TicTacToe/Bingo `run_sample.sh:24`의 `kill -INT`, `:27-43`의
2초 대기 및 SIGKILL, ZoneWorld `run_sample.sh:84-89`의 즉시 SIGKILL이다.
개별 runner에 signal·대기 수치를 각각 수정하는 대안과 공통 종료 함수를 사용하는 대안을
비교해, 종료 순서를 한 곳에서 소유하는 후자를 선택했다.

| 변경 파일 | 결과 |
|---|---|
| `framework/languages/dotnet/samples/redis-common.sh:39,83-114` | 기존 종료 추적기가 SIGTERM을 추적한다. 공통 함수가 역순 SIGTERM → 30초 관찰 → 필요한 경우 SIGKILL → wait를 수행한다. |
| `samples/{TicTacToe,Bingo,SupportChat,ShoppingMall,DeliveryDispatch,GameQuest}/run_sample.sh` | 복제된 cleanup process loop를 공통 함수 호출로 바꾼다. |
| `samples/ZoneWorld/run_sample.sh:84` | EXIT cleanup도 공통 함수를 사용한다. 시나리오의 의도적인 node 장애 주입은 그대로다. |
| `samples/GameQuest/run_sample.sh:322-334`, `redis-common.sh:116-125` | owner-loss 시나리오에서 Mission의 exit 137을 확인하고 PID를 제거한다. 완료한 client도 제거한다. ZoneWorld의 기존 `remove_owned_pid`를 공통 모듈로 이동해 재사용한다. |
| 이 결과 문서 | 진단·계약 근거·검증·잔여 실패를 기록한다. |

30초는 Framework deadline의 증가가 아니라 **runner가 이미 허용된 shutdown deadline 전에
process를 종료하지 않도록 관찰 시간을 맞춘 값**이다. §14의 deadline 초과 후 bounded
teardown에는 고정된 추가 초 단위 상한이 명시되어 있지 않다. Java의 90초를 복사하거나
임의의 여유 시간을 추가하지 않았다. 이번 정상 종료 검증을 deadline을 모두 소비하는
teardown의 상한 검증으로 확대 해석하지 않는다.

`run_samples.sh`의 status file 판정과 137 실패 반환은 유지했다. SIGTERM을 무시하는 별도
Python process로 실제 30초 관찰 뒤 exit 137과 role `stubborn`의 단일 기록을 확인했다
(`sigkill-probe.log`, `sigkill-probe.teardown`, `verify-sigkill.sh`). 의도적인 scenario SIGKILL은
cleanup 밖에서 발생하면 teardown 실패로 기록하지 않는 기존 구분도 확인했다.

첫 aggregate는 GameQuest 시나리오가 이미 SIGKILL한 Mission PID를 cleanup 목록에 남겨,
나중의 `wait`가 의도된 exit 137을 cleanup 실패로 보고했다. `aggregate.trace:29786`은
시나리오의 kill, `:32517-32541`은 cleanup에서의 중복 wait와 실패 기록이다. 이 원인은
runtime teardown 실패와 다르며 process를 끝낸 시나리오가 PID 소유도 해제하도록 수정했다.
SIGKILL 판정에 GameQuest 예외나 PID 무시 목록을 추가하지 않았다. 이 변경 뒤 GameQuest는
통과했고, 추가된 변경 범위 때문에 focused 검증 후 최종 aggregate를 실행했다.

PowerShell 경로는 조사만 했다. `sample_runner.ps1:373`은 `Start-Process`, `:402`는
`CloseMainWindow()`, 뒤에는 20×100 ms 대기와 `Kill(true)`를 사용한다. 이번 실행·수정 대상은
요청된 Linux Bash aggregate다. PowerShell의 종료 동작을 검증하거나 수정했다고 주장하지 않는다.

## 패키지와 실행 환경

요청된 TMPDIR·ZLINK_LIBRARY_PATH·컴파일 환경을 사용하고 package SHA별 NuGet cache로 실행했다.
모든 .NET build/test/sample 명령은 `/tmp/zlink-dotnet-gate.lock`을 `flock -w7200`으로 획득했다.
`--artifacts-path`, `ulimit -v`는 사용하지 않았다. 명령은 증거 디렉터리의 `env.sh`,
`verify-focused.sh`, `verify-gate.sh`, `verify-aggregate.sh`에 있다.

- `Systems.Zlink.0.17.0.nupkg` SHA256:
  `be4ab2bbff665e04886c139dbab712da71b3c7fdcef412ab6b795fa816ad5f3a`.
- `core/build-dev/lib/libzlink.so` SHA256:
  `98f3499696009ee5d43a1680ab5423c306d28af7592c1ca48fb40f3ee20773eb`.
- 실제 role의 `/proc/maps`에는 sample output의 `runtimes/linux-x64/native/libzlink.so`가
  로드되어 있다. 이 파일의 SHA256이 위 local Core library와 같은 것을 확인했다.
  최종 검증 뒤에도 package와 library hash가 같았다(`final-library-hashes.txt`).

## 검증 결과

| 검증 | 결과 | 증거 |
|---|---|---|
| TicTacToe 3회 | **3/3 exit 0**, 역할 12/12 exit 0, SIGKILL 없음 | `TicTacToe-{1,2,3}.{log,exit,trace}`, 각 evidence 디렉터리 |
| Bingo 3회 | **3/3 exit 0**, 역할 21/21 exit 0, SIGKILL 없음 | `Bingo-{1,2,3}.{log,exit,trace}`, 각 evidence 디렉터리 |
| 역할 exit 검증 | **33/33 exit 0** | `focused-role-exits.txt`; shell trace의 실제 `builtin wait` 결과 |
| SIGKILL 실패 보존 | **통과** | `sigkill-probe.log`; 비협조적 역할 exit 137을 한 번 기록 |
| SampleRegressionTests 1회 | **150/157**, 아래 기존 실패 7건 | `sample-regression.log`, `test-results/sample-regression.trx` |
| GameQuest·ZoneWorld·SIGKILL focused regression | **26/26** | `runner-focused.log`, `test-results/runner-focused.trx` |
| 첫 aggregate | TicTacToe부터 DeliveryDispatch까지 통과, GameQuest의 완료 PID 중복 wait로 **137** | `aggregate.{log,trace,exit}`, `aggregate/evidence/` |
| PID 소유 해제 수정 후 GameQuest·ZoneWorld | GameQuest 통과. ZoneWorld **ZW-G4 기능 실패**, cleanup SIGKILL 없음 | `remaining.{log,trace,exit}`, `remaining/evidence/` |
| 최종 전체 7개 sample aggregate | **6/7**, ZoneWorld ZW-G4 기능 실패로 **exit 1**. Cleanup 역할 **31/31 exit 0**, cleanup SIGKILL **0** | `final-aggregate.{log,trace,exit}`, `final-role-exits.txt`, `final-aggregate/evidence/` |
| Bash syntax·whitespace | 통과 | 변경된 Bash 파일의 `bash -n`, `git diff --check` |

## BLOCKERS

ZoneWorld는 별도 실행과 최종 aggregate 모두 ZW-G4에서 `CrashRelocationProbeRes`를 기다리다 끝났고,
interrupted operation이 `Unavailable`로 끝나지 않았다는 기존 검증에 실패했다
(`remaining.log:42-44`, `final-aggregate.log` 끝부분). Trace에서는 node 장애 주입의 의도된 exit 137과 client exit 1만
비정상 종료이며, cleanup에 남은 역할 4개는 모두 exit 0이다. 이 기능 실패를 숨기거나
응답 timeout·assertion을 변경하지 않았다. ZW-G4 child에서 중단되어 이후 ZoneWorld 시나리오는
검증되지 않았다. 같은 원인으로 전체 gate를 더 반복하지 않았다.

Process exit 0을 모든 runtime termination 결과가 `Stopped/None`이라는 뜻으로 계산하지 않았다.
최종 evidence에는 TicTacToe play-b, Bingo play-a, SupportChat support, DeliveryDispatch
courier-node-1/courier-node-2/customer-gateway, GameQuest api-b의
`ForceStopped/TeardownFailed`와 ShoppingMall workflow-a의
`ForceStopped/DeadlineExceeded`가 있다. 해당 로그는 `final-aggregate/evidence/*/logs/`에
보존했다. 모두 process exit 0이며 SIGKILL이나 process 종료 정지는 아니다. 이 결과의 내부
원인은 이번 runner 수정에서 확정하지 않았다.

SampleRegressionTests의 다음 실패는 cleanup 변경과 무관하다. 기준 revision
`1a68f6ec6da94d08b6c15fdbcf324e43153d5a0f`와의 대조를
`regression-baseline-check.txt`에 보존했다. 해당 runner의 cleanup 뒤 본문은 기준 revision과
같고, 누락된 문자열·옛 spec 경로·금지 문자열도 기준 revision에 같은 상태다.

| 실패 test | 원인 |
|---|---|
| `TicTacToe_Runner_Verifies_Client_And_Server_Evidence` | runner에 `stream-inbound sample=TicTacToe` 문자열 없음 (`TicTacToeRegressionTests.cs:327`) |
| `SupportChat_Runner_Uses_Isolated_Docker_Redis_And_Location_Store` | runner에 `supportchat-server-evidence=completed` 없음 |
| `ShoppingMall_Runner_Uses_Isolated_Docker_Redis_And_Redis_Stores` | runner에 `shoppingmall-server-evidence=completed` 없음 |
| `DeliveryDispatch_Runner_Uses_Isolated_Docker_Redis_And_Location_Store` | runner에 `topology=ready` 없음 (`DeliveryDispatchRegressionTests.cs:111`) |
| `DeliveryDispatch_Contracts_Match_Common_Role_Model` | runner에 `deliverydispatch courier-session: bound courier=courier-a` 없음 (`DeliveryDispatchRegressionTests.cs:219`) |
| `Sample_Health_Checks_Use_Location_Readiness` | 변경하지 않은 `OrderWorkflowServerHostFactory.cs:108` 등에 있는 `IZLinkLocationRuntimeQuery`를 금지하는 assertion (`Regression.cs:321`) |
| `DotNet_Docs_Keep_Actor_Destroy_Entry_Owned` | `Regression.cs:454`가 옛 `spec/server/11-spot-model.ko.md`를 읽어 FileNotFoundException |

기존 failure를 없애기 위한 assertion·fixture·문서 변경은 하지 않았다. `main`에서 수정했으며
commit하지 않았다. 기존 사용자 변경과 작업 중 다른 작업자가 갱신한 파일은 보존했다.

# g1646m — framework/1646-registry-deadline에 origin/main merge + 리뷰 반려(F1~F3) 수정

worktree: /root/worktree/zlink-1646-registry-deadline (branch framework/1646-registry-deadline)
1차 merge 대상: origin/main 90aafafaff (merge-base 0f8f11dbd0, branch tip 8a9a9e314f)
2차 merge 대상: origin/main 9fb223eb3b (#1804·#1805·#1806, Java만)

## 커밋
| 커밋 | 내용 |
|---|---|
| 4dcfd817fe | Merge origin/main(90aafafaff) — 준비된 메시지, `# Conflicts` 제거, 마지막 줄 Co-Authored-By 확인 |
| 41ff29daf3 | fix(framework): count the deadline management budget from entry (F1, 세 언어) |
| b7122613bf | test(framework): drop deadline tests that read implementation source (F2) |
| 1bd75f44eb | test(framework): drive deadline budget tests with an injected clock (F3) |
| 821116b63f | Merge origin/main(9fb223eb3b) — 충돌 없음, 본문 `Refs #1646` + Co-Authored-By |

1차 merge 뒤 의미 충돌(컴파일 오류 등)은 없어 별도 수정 커밋은 없다. 2차 merge는 ZLinkServiceOperationRegistry를 건드리지 않았다
(main 변경: ZLinkJavaRawMeshNode/RawSpotNode, ZLinkSpotActivation*, ZLinkStatusPublisher, ApplicationJobContext와 그 시험).

## 1차 merge: 충돌별 최종 형태와 근거 (ZLinkManagedMeshNode.cs, 3 hunk)

### 1. RequestToSpotDirectAsync 로컬 경로 (merge 커밋 기준 L4181)
- main ca4e84e968(#1745): `StartDetached(ExpireOperationAsync…)` 위에 "Caller cancellation competes…" 주석 추가, 등록 콜백이 `operation.Cancel(cancellationToken)`.
- 브랜치 183188ba79/4ad8760878(#1646): `StartDetached(ExpireOperationAsync)` 제거, `CreateOperationAsync(…, managedTimeout: effectiveTimeout)`로 timer wheel 등록.
- 최종: StartDetached 없음(브랜치) + main 주석과 `Cancel(cancellationToken)` 콜백 유지. `ExpireOperationAsync`는 파일에서 0건.

### 2. DetachReceiveWake / ReceiveLoop (merge 커밋 기준 L5442, L5362, L5396)
- git이 엇갈려 맞춘 hunk: HEAD 쪽은 base의 옛 receive loop 본문(`_reservedRawApplicationAdmission`이면 PollIn 재무장 + 브랜치의
  `Wait(events, OperationWaitBound())`, `ExpireOperations()`), main 쪽은 DetachReceiveWake의 `wake.Stop();`.
- main 394275bc92(#1638 permit grant wake)가 loop를 poll source 2개 + wake timer 구조로 바꾸고 PollIn 재무장을 DrainRawSocket으로 옮겼다.
- 최종: DetachReceiveWake는 main 그대로(`wake.Stop()`). 브랜치 동작은 main의 새 ReceiveLoop 위에 얹음 —
  `_poller!.Wait(events, OperationWaitBound())`(L5362), `ProcessInfrastructure(now)` 뒤 `ExpireOperations()`(L5396). 옛 재무장 코드는 버림(main이 이동).

### 3. PendingOperation.Cancel (merge 커밋 기준 L12992)
- main ca4e84e968: `_terminal` CAS 안에서 `_timeout.Cancel()` 후 caller 취소면 `TrySetCanceled(callerCancellation)`, 아니면 Terminated.
- 브랜치 183188ba79: `_timeout`/`_terminal` 제거, `_lifetime`+`Complete()`. 완료 권한은 `TryRemoveOperation`(원자적 take) 한 곳 — 모든 `Cancel` 호출이 take 성공 뒤에만 부른다(L1657, L4207, L5028, L9626, L10797, L10809, shutdown L3302는 dictionary 비운 뒤 1회).
- 최종: `Complete();` 후 main의 분기(`callerCancellation.IsCancellationRequested` → `TrySetCanceled`, 아니면 Terminated) — 시그니처 `Cancel(CancellationToken callerCancellation = default)` 유지.
- main 쪽 새 `TryComplete()` 사용처(5025/5157/10960)는 브랜치 형태로 auto-merge되어 0건.

## 리뷰 반려 수정 (review-1646.md)

### F1 — 관리 작업 시간 상한을 진입 시점부터, 방문마다 검사 (41ff29daf3)
- Java `ZLinkServiceOperationRegistry.expire()` L313~: `startedNanos`를 진입 때 읽고 `while (nanoTime - startedNanos < budget)`로 매 방문(rounds 감소 포함) 전 검사. 첫 만료 때 시작하던 줄 삭제.
- C++ `operation_registry.cpp` expire() L503: `const auto started = _now ()`, `while (_now () - started < receive_batch_time)`. 시험이 상한 경과를 결정적으로 만들도록
  `operation_registry_t (now_t now)` 생성자 추가(L244, 기본 생성자는 `&clock_t::now`로 위임). `_deadline_origin`도 같은 시계로 읽는다.
- .NET `TakeExpiredOperation`(while 안에서 rounds를 상한 없이 넘김) → `VisitOperationDeadline()`(L11348): 한 번에 한 방문, `(Expired, Visited)` 반환.
  `ExpireOperations()`(L11382): 진입 때 `_deadlineClock.Elapsed`, 방문마다 lane 한 번 + 상한 검사. 상한 시계를 Stopwatch에서 주입된 deadline 시계로 바꿈(운영은 TimeProvider.System).
- 다음 회전은 기존 `_deadlineCursor`/`deadline_cursor`에서 이어 간다. 새 상태·timer 없음(C++ `_now`는 시계 주입점; Java의 `nanoTime`과 같은 역할).
  판정 위치 수: 수정 전 언어당 2(첫 만료 시점 상한 시작 + 루프 조건) → 수정 후 1(루프 조건).
- 수정 전 실패(rounds만 있는 8개 뒤에 만료 1개, 시계가 읽을 때마다 상한/4씩 진행):
  - Java `roundsOnlyVisitsYieldAtTheManagementBudgetAndResumeFromTheCursor` — 수정 전 `assertEquals(0, registry.expire(tick))` 실패(첫 회전이 8개를 상한 없이 넘기고 만료까지 처리).
  - C++ `yielding` 블록 — 수정 전 `Assertion 'yielding.expire (tick) == 0' failed`.
  - .NET `OperationDeadline_RoundsOnlyVisitsYieldAtTheBudgetAndResumeFromTheCursor` — 수정 전 `Expected: 9, Actual: 8`.
  - 수정 뒤 세 시험 통과: 첫 회전은 만료 없이 양보, 이후 회전이 cursor에서 이어 만료, rounds 남은 8개는 257ms 전까지 유지(처음부터 다시 훑으면 rounds가 두 번 줄어 조기 만료 → 실패).
- .NET `OperationDeadline_PreservesRoundsAndResumesABudgetedSlot`의 `Thread.Sleep(3)`은 상한 시계를 주입 시계로 바꾼 같은 커밋에서
  `clock.Timestamp += 1ms`로 바꿨다(그대로 두면 이 커밋에서 시험이 깨지므로 F3의 .NET 항목은 F1 커밋에 포함).

### F2 — 소스 문자열 검사 시험 제거 (b7122613bf)
- Java `deadlineLinkAndUnlinkUseFixedPointerWork`, C++ main() 첫 블록(소스 파일 읽기, `<filesystem>`·`<iterator>` include 제거),
  .NET `OperationDeadline_DoesNotAllocateARequestTimer`, `OperationDeadline_LinkAndUnlinkUseFixedPointerWork`(+ 쓸모없어진 `ManagedMeshSourcePath`) 삭제.
- 동작 시험(방문 수 계수)으로 바꾸지 않은 이유: 계수를 노출하려면 새 내부 상태가 필요해 §3(새 상태 금지)과 충돌. 휠 동작은 기존 deadline/rounds/budget 시험이 덮는다.

### F3 — 속도·sleep 의존 시험을 주입 시계로 (1bd75f44eb, .NET은 41ff29daf3)
- C++ budgeted 블록: 50000건/실시간 → 10건/주입 시계(읽을 때마다 상한/4) — 첫 회전 정확히 3건, 이후 cursor에서 재개, 중간 unregister·재등록 후 합계 10.
- Java `oneMaintenanceTaskServesEveryPendingDeadlineAndExpiresAtTheBoundary`: 1차 merge 검증에서 1회 실패(L124 부근). 원인: 실제 scheduler의
  1ms 고정 주기 maintenance가 시험의 명시적 `expire`와 동시에 돌아, 시험 쪽이 slot 중간에 cursor를 남긴 사이 maintenance가 그 slot 항목을 가져감
  (cursor가 있으면 nowTick 비교 없이 진행). 운영에서는 maintenance 한 스레드만 `expire`를 부르므로(고정 주기, 겹치지 않음) 시험 전용 경쟁 —
  schedule 횟수 assertion 뒤 `scheduler.shutdownNow()`(다른 시험과 같은 방식). assertion 변경 없음.

## 시험
`source ~/.bashrc && bash scripts/gate/rebuild-dev.sh g1646m`(1차 merge 커밋 4dcfd817fe에서, 성공) 뒤 실행. 실행기: gate `dotnet_env`와 같은 환경.
- C++: `framework/languages/cpp`에서 main-base와 같은 설정(`build/linux-ninja-release`, Release, vcpkg toolchain, BUILD_TESTS=ON, SAMPLES/CROSS_LANGUAGE=OFF, 로컬 core/cpp prefix) → `cmake --build` → `ctest --output-on-failure -j4 --timeout 600`
- Java: `./gradlew --no-daemon test contractTest --continue`
- .NET: `dotnet test tests/Zlink.Framework.UnitTests --blame-hang --blame-hang-timeout 10m`, `dotnet test tests/Zlink.Framework.ContractTests`
- Node: 이 브랜치가 바꾸지 않아 생략.

| 시험 | 1차 merge 4dcfd817fe | 최종 821116b63f | main-base 4bf652482f |
|---|---|---|---|
| C++ ctest | 168/169 (실패: test_cpp_stream_connector_install_consumer) | 168/169 (같은 install_consumer lz4 실패만) | 같은 시험 실패 |
| Java test+contractTest | 2502 중 1 실패(oneMaintenance…, 위 F3) + locations-redis 19 실패 | BUILD SUCCESSFUL(실패 0) | redis 19 실패 동일 |
| .NET UnitTests | 2974/2974 | 2973/2973 (F2 −2, F1 +1) | - |
| .NET ContractTests | 96/96 | 96/96 | - |

- `test_cpp_stream_connector_install_consumer`: 재배치된 install consumer configure에서 `Could NOT find lz4` — 환경(시스템 lz4 개발 파일 없음). main-base에서도 같은 실패.
- locations-redis 19건: 1차 실행에서 `ZLINK_REDIS_LOCATION_ENDPOINT`를 넣었으나 이 머신에 Redis 서버가 없음. main-base에서 같은 env로 `:zlink-framework-locations-redis:test --rerun` → 같은 19건 실패. 최종 실행은 env 없이(해당 live 시험 skip).
- 최종 C++ 첫 시도 2회는 머신 부하로 컴파일러가 죽음(ICE segfault, OOM kill — dmesg `Out of memory: Killed process cc1plus`), `-j3`로 다시 빌드.
- 새 실패: 0 (위 두 건은 main-base에서도 실패, Java oneMaintenance는 브랜치 시험의 경쟁으로 F3에서 고침).

## 그 밖
- 작업 트리에 남은 변경 `framework/languages/unity/com.zlink.stream-connector.webgl/Plugins/WebGL/zlink-stream-connector.jspre`는 rebuild-dev의 node build가 다시 만든 산출물로, 커밋하지 않았다. untracked `framework/languages/engines/Server/.run/`, `zlink-work/`도 그대로.
- C++ clang-format 18이 이 머신에 없어 C++ 변경은 수동으로 주변 형식에 맞춤(gate format-check에서 확인 필요). Java는 google-java-format 1.36.1 --aosp, .NET은 CSharpier로 해당 파일만 포맷.
- 점검: 발견 1(Java 시험 경쟁, 고침), 넘긴 항목 0.

## 재리뷰 후속 (F4, format, Redis·lz4 재시험, 정리) — HEAD 821116b63f, 새 커밋 없음

### F4 (.NET lane turn 묶기) — 멈춤, 결정 필요
- 지시대로 구현해 봤다: lane turn 한 번(`TakeExpiredOperations`) 안에서 진입 시점부터 방문마다 상한 확인, 만료 operation은 재사용 필드
  `List<PendingOperation> _expiredOperations`에 모으고 lane 밖에서 Complete·결과 전달, tuple 반환 제거. 패치: /root/jobs/fx/cx/g1646m-f4-list-experiment.patch
- 결과: `OperationDeadline_RoundsOnlyVisits…` 통과, **`OperationDeadline_PreservesRoundsAndResumesABudgetedSlot` 실패**(MeshNodeShutdownSealTests.cs:142, Expected 2 / Actual 0).
  이 시험은 첫 만료의 Complete(token 콜백) 비용이 상한을 다 쓰면 나머지 둘이 다음 회전으로 넘어가야 한다고 본다. 목록 방식은 세 건을
  lane 안에서 모두 꺼낸 뒤(그동안 시계는 그대로) lane 밖에서 Complete하므로 Complete 비용이 상한에 들어가지 않는다. 꺼낸 operation은 되돌릴 수 없다.
  → "목록 방식"과 "PreservesRounds 시험 유지"는 함께 성립하지 않는다. 실험 변경은 되돌렸다(작업 트리 깨끗).
- 선택지:
  A. 목록 방식 채택 + 시험 의미 변경: 상한은 lane 안 방문(꺼내기)만 덮고, Complete·결과 전달은 상한 밖. PreservesRounds 시험은 "만료 비용으로 상한 소진"
     대신 주입 시계로 방문 중 상한을 소진시키는 형태로 바꿔야 함(구현에 맞춘 assertion 변경에 해당할 수 있어 감독 판단 필요).
  B. 만료마다 lane 한 번: lane turn 안에서 rounds-only 방문은 루프로 넘기고 첫 만료에서 반환, lane 밖에서 Complete 후 상한 확인·재진입.
     lane 진입 = 만료 수 + 1(rounds-only 수백 번 진입 문제는 해소), 두 시험 모두 유지. 단 진입 시점 기준 상한을 lane 안에서 보려면
     시작 시각을 넘길 필드 하나(TimeSpan)가 필요(목록 필드 대신 새 상태 1개) — lane API(`RunAsync(Func<T>)`)에 상태 인자가 없고 closure는 회전마다 할당.

### C++ format
- `scripts/format/format.sh --check cpp`(gate의 format-check와 같은 스크립트, clang-format-18): 위반 0.
- 전체 `format.sh --check`: rc=1, 위반 1건 `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/FrameworkJsonProfileTests.cs` — origin/main과 동일한 파일(d9919de2f0, 이 브랜치 변경 아님). 고치지 않음.

### 재시험 (HEAD 821116b63f, Redis 127.0.0.1:6379 + ~/.bashrc `ZLINK_REDIS_LOCATION_ENDPOINT`, liblz4 설치 후)
| 시험 | branch | main-base 4bf652482f |
|---|---|---|
| C++ ctest(unit·contract, 169) | 168/169 — install_consumer 통과(lz4 해결). 실패 1: test_cpp_framework_spot_close_conformance 안 `ActorMaintenanceJoin.SameNodeMembershipPreservesReturnedActorUntilSourceCleanup`(actor_maintenance_join.cpp:382). 단독 반복 5회 통과 | 같은 시험 단독 실행에서 같은 위치(:382) 실패 → 기존 불안정, 새 실패 아님 |
| Java `test --rerun contractTest --rerun --continue`(Redis 포함) | BUILD SUCCESSFUL, 결과 XML 3061건 실패 0·skip 0 (locations-redis live 시험 포함 37건 통과) | - |
| .NET UnitTests | 2973/2973 | - |
| .NET ContractTests | 96/96 | - |
- 새 실패: 0.
- 주의: 첫 Java 실행은 Gradle이 test를 UP-TO-DATE로 건너뛰어 `--rerun`으로 다시 돌렸다.

### 정리
- rebuild/시험이 다시 만든 Unity `.jspre`는 `git checkout --`로 되돌렸다(두 번, 시험이 재생성). `framework/languages/engines/Server/.run/`는 이 job의 C++ 시험이
  만든 것(생성 15:11, ctest 실행 중)이라 지웠다. 남은 untracked는 `zlink-work/`(gate 로그)뿐.

## F4 — B안 구현 (bd0ebd3964 perf(dotnet): enter the operation lane once per expired deadline)
- ZLinkManagedMeshNode.cs `TakeExpiredOperation()`(L11352): lane turn 한 번 안에서 rounds-only 방문을 이어 처리, 방문 뒤마다 상한 확인, 첫 만료 operation에서 반환.
  `ExpireOperations()`(L11395): 진입 때 `_deadlineVisitStartedAt`을 기록하고, 만료 하나마다 lane에 한 번 들어간다. Complete·결과 전달은 lane 밖에서 하고,
  그 뒤 상한을 확인한다(완료 전달 시간 포함, §4.2). lane 진입 수 = 만료 수 + 1.
- 상태: 필드 `_deadlineVisitStartedAt`(TimeSpan) 하나 추가, per-visit tuple 반환과 `_visitOperationDeadline` 제거. 캐시된 `Func<PendingOperation?>` 그대로라 호출마다 closure 없음. lock·timer 추가 없음.
  상한 판정은 `DeadlineBudgetSpent()` 한 곳(lane 안 rounds 방문 뒤와 lane 밖 완료 뒤 두 호출).
- 시험: `OperationDeadline_RoundsOnlyVisits…`, `OperationDeadline_PreservesRounds…` 포함 OperationDeadline_/OperationTerminal_ 4건 통과(시험 변경 없음).
  .NET UnitTests 2973/2973, ContractTests 96/96 — F4 전(821116b63f)과 같고 main 대비 새 실패 0.
- 작업 트리: 남은 untracked `zlink-work/`(gate 로그)뿐.

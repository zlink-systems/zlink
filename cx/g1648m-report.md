# g1648m — framework/1648-receive-path에 origin/main merge

worktree: /root/worktree/zlink-1648-receive-path (branch framework/1648-receive-path)
merge 대상: origin/main 90aafafaff (merge-base 1552747efc, branch tip 85d0eceeb8)

## 커밋
| 커밋 | 내용 |
|---|---|
| cecf07a0f6 | Merge origin/main into framework/1648-receive-path (준비된 메시지, `# Conflicts` 제거, 마지막 줄 Co-Authored-By 확인) |
| fc4a6c67ab | fix(java): rename drain locals shadowing the merged entry parameter (의미 충돌: 컴파일 오류) |
| 14666098cd | test(dotnet): invoke the raw receive seam with its merged signature (의미 충돌: reflection 시그니처) |

## 충돌별 최종 형태와 근거

### 1. Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs — main 그대로
- main d2c7cab990(#1745, "the raw receive source owns every multicast child permit")이 pump의 admission 획득
  (`TryTakeApplicationAdmissionAsync`, `WaitForApplicationAdmissionAsync`, `_reservedApplicationAdmission`,
  `RequiresApplicationAdmission`, admissions 배열)을 모두 지웠다. 각 application record는 source가 mailbox에 넣기 전에 얻은 permit을 들고 온다.
- 브랜치 1620452aa7은 바로 그 지워진 pump admission 경로를 reservation(count)으로 바꾼 것이라 대상 코드가 없다.
  브랜치의 나머지 의도(`nodeRoutes`를 최대 건수로 미리 할당하지 않기)는 main의 lazy `ref nodeRoutes`가 이미 충족한다.
- 결과: `git show MERGE_HEAD:<file>`과 동일. DrainClaimAsync는 pump.cs:378 부근.

### 2. Runtime/Service/ZLinkManagedMeshNode.cs — 6개 hunk
- L43-56 상수: 양쪽 유지. 브랜치 `ReadyDomainOrder`(1620452aa7) + main `ReceiveWakePollerSlot`·`ReceivePollSourceCount`·`WakeFireCount`·`ReceiveWakeInterval`(394275bc92 #1769 permit grant wake).
- DrainReadyAsync(L2192~): 브랜치 구조(정렬 버퍼 없는 도메인별 열거, `ReadyDomainOrder`, `entries.Reset()`) 위에 main 시그니처 적용 —
  `mailbox.TryClaimAsync(canClaim)`(인자 1개), `MeshReadyRecord(OwnerKind, Domain, SpotId, Actor)`(4개). main이
  `RequireReservedApplicationAdmission`/`AvailableRecords`/`ApplicationAdmissionReserved`를 지웠으므로 그 인자들은 제거.
- ReceiveLoop(L5327~)·DetachReceiveWake: main 그대로(poll source 2개, wake timer, `try/finally DetachReceiveWake`, `wake.Stop()`).
  브랜치 쪽 hunk는 base의 옛 loop 본문(`_reservedRawApplicationAdmission`이면 PollIn 재무장)이었고 main이 이를 DrainRawSocket 안으로 옮겼다.
  main의 `admissions` 배열 할당만 브랜치 방향대로 제거(아래 reservation 사용).
- DrainRawSocket(L5431)/ReceiveRawBatch(L5480)/TakeRawAdmission(L5521): **main 구조 + 브랜치 reservation**.
  - main(d2c7cab990, 394275bc92) 동작 유지: queue 없음이면 바로 수신, 예약 permit이 있으면 PollIn 재무장, 각 permit은 record 하나 또는
    보류 중 multicast fanout의 다음 child 하나를 admit(`_pendingRawMulticast` → `ContinueRawMulticast`), `count == 0 || _pendingRawMulticast is not null`이면
    infrastructure 이벤트만 남기고 waiter 하나 등록.
  - 브랜치(1620452aa7) 유지: 회전당 `queue.TryReserveBatch`로 count만 잡고, lease는 실제 수신된 record 또는 실제 posting되는 child에서만
    `reservation.Take()`로 만든다. 남은 예산은 `reservation.Dispose()`가 반환. main의 `TryAcquireBatch`/`ReleaseBatch(admissions)`는
    브랜치가 queue API에서 이미 제거해 쓸 수 없다(ZLinkApplicationJobQueue.cs는 main 변경 없이 브랜치 쪽으로 auto-merge).
  - lease 생성 시점만 바뀜: main은 Recv 전에 lease를 쥐고 미수신이면 dispose, 병합본은 Recv 성공 뒤 Take. permit 수 계산은 같다.
- ContinueRawMulticast finally: main 그대로(`admission?.Dispose()`, 남은 child를 `_pendingRawMulticast`에 보관). 브랜치 hunk(first/reservation dispose)는 DrainRawSocket finally로 이동.
- ProcessInfrastructure(L9320): 충돌 없음, 브랜치 형태(node lane 한 turn에서 membership·liveness 판정 모으기). main은 이 함수를 바꾸지 않았다.

### 3. tests/.../ServiceRuntimeFoundationTests.cs — 양쪽 유지
- main의 새 시험 `Managed_mesh_logical_multicast_posts_each_child_with_its_own_receive_permit`와 helper `ClaimOnlyMulticastChildAsync`(d2c7cab990) 그대로(L3195~).
- 그 뒤 브랜치 85d0eceeb8의 `[Theory] Managed_mesh_shutdown_keeps_application_permit_until_the_admitted_multicast_child_finishes(int publishCount)`(L3308~).
  main은 이 시험 본문을 바꾸지 않았다(base↔main diff 0).

## 추가(의미 충돌) 커밋
- fc4a6c67ab Java `ZLinkSerialExecutionQueue.scheduleDrainIfNeeded`(L1283, L1291): main(09da8bbc52 이전 #1759 계열)이 매개변수 이름을
  `entry`로 바꾸었고, 브랜치 c7228797d7이 옛 `drainScheduled()` 본문을 이 메서드에 inline해 `Entry entry` 지역 변수가 중복 선언됨 → 지역 변수만 `next`로 개명. 동작 변화 없음.
- 14666098cd .NET `MeshReceiveWakeTests.PermitGrantResumesReceiveOwnerWithoutAPollTick`(main 394275bc92 시험)이 reflection으로
  `DrainRawSocket(token, lease[])`를 호출 → 병합본 시그니처 `DrainRawSocket(token)`로 인자만 맞춤. assertion 변경 없음.

## 시험
전부 `source ~/.bashrc && bash scripts/gate/rebuild-dev.sh g1648m` 뒤 실행(gate common.sh `dotnet_env`).
rebuild 중 "JAVA REFRESH FAILED"는 위 Java 컴파일 오류 때문이었고, 수정 뒤 `./gradlew compileJava compileTestJava compileKotlin compileTestKotlin` 성공.

| 시험 | branch | main-base 4bf652482f |
|---|---|---|
| `dotnet build tests/Zlink.Framework.UnitTests` | 성공 | - |
| `dotnet test tests/Zlink.Framework.UnitTests --filter 'FullyQualifiedName!~CanonicalActorJoinIngressReplyTests' --blame-hang --blame-hang-timeout 10m` | 2948/2951 (실패 3, 아래) | - |
| `dotnet test tests/Zlink.Framework.UnitTests --filter 'FullyQualifiedName~CanonicalActorJoinIngressReplyTests' ...` | 23/23 | - |
| `dotnet test tests/Zlink.Framework.ContractTests` | 96/96 | - |
| java `./gradlew --no-daemon test contractTest --continue` (jvm lock) | BUILD SUCCESSFUL | - |
| 재실행 `--filter MeshReceiveWakeTests|RuntimeConformanceFixtureTests.Route_mesh_placement_counts` (14666098cd 뒤) | MeshReceiveWake 3/3 통과, Route_mesh 실패 | 4/4 통과(1회) |
| `Route_mesh_placement_counts_follow_the_reporting_mesh_node` 반복 | 3회 중 2 실패 | 5회 중 3 실패 + 3회 중 1 실패, 같은 메시지 |

실패 분류:
- `MeshReceiveWakeTests.PermitGrantResumesReceiveOwnerWithoutAPollTick(failRearm: False/True)`: 병합 시그니처 불일치(TargetParameterCountException) → 14666098cd로 해결, 재실행 통과.
- `RuntimeConformanceFixtureTests.Route_mesh_placement_counts_follow_the_reporting_mesh_node`
  ("user-spot-on-first-mesh-node: The claimed MeshNode descriptor could not enter Serving state."): main-base에서도 같은 메시지로 간헐 실패 → 기존 flake, 새 실패 아님.

**새 실패: 0.** 로그: /tmp/claude-0/-root/3a9c19c5-bb10-46f5-98c2-cf9785e10af4/scratchpad/br/

## 기타
- rebuild-dev의 node build가 framework/languages/unity/com.zlink.stream-connector.webgl/Plugins/WebGL/zlink-stream-connector.jspre를 다시 생성해 worktree에 미커밋 변경으로 남아 있다(이 job의 변경 아님, 커밋하지 않음).
- main-base worktree에는 gate common.sh가 만든 로그 디렉터리 zlink-work/gates/g1648m-base만 생겼다(소스 수정 없음).
- push·PR·보드 갱신 안 함.

## 추가 merge: origin/main 9fb223eb3b (#1804·#1805·#1806, Java만)
감독자 지시로 위 작업 뒤 `git fetch origin main && git merge --no-ff origin/main` 한 번 더.

- 커밋 **fa4e1d6251** "Merge origin/main into framework/1648-receive-path" (부모 14666098cd, 9fb223eb3b; 본문 `Refs #1648`, 마지막 줄 Co-Authored-By).
- 이번 main 커밋들은 ZLinkSerialExecutionQueue·ZLinkActorDispatchSerials를 건드리지 않아 그 두 파일은 충돌 없이 브랜치 상태 그대로다
  (commonPool 기본값은 이번 merge로 새로 들어오거나 빠지지 않았다; 되돌리기는 감독자 별도 지시 대기).
- 충돌 1개 파일: framework/languages/java/.../runtime/binding/ZLinkJavaRawMeshNode.java, 3 hunk.
  - close(): 브랜치 c7228797d7 순서 유지(pump `shutdownNow()` → … → `port.close()` → `awaitExecutorTermination(pump)`, 등록 socket 종료로 pump wait를 깨움).
    main 54d52a24cf(#1805)의 "pump가 멈춘 뒤 미완료 fanout child 정리"(`pendingRawMulticast` close)는 그 의미대로 브랜치의 awaitExecutorTermination **뒤**로 옮겼다(fanout.close는 Message close만 한다).
  - pumpTurn(): 브랜치 형태(`waitTimeout` = 이전 회전 `tickLiveness`가 돌려준 liveness deadline, `finishRequestedIntentCloses`, 회전 끝 `tickLiveness`) 위에 main #1805 조건을 얹음 —
    `pendingRawMulticast != null && pendingReceiveAcquire.get() == null`이면 wait `Duration.ZERO`, 아니면 `waitTimeout`; drain 조건 `READABLE || pendingRawMulticast != null`.
    main의 `ingressWaitTimeout`(브랜치가 제거한 필드)과 main 쪽 회전 앞 `tickLiveness(now)`는 들이지 않았다.
- 검증: `./gradlew compileJava compileTestJava compileKotlin compileTestKotlin` 성공, `scripts/format/format.sh --check java` 통과
  (`--check dotnet`은 main 소유 FrameworkJsonProfileTests.cs 한 곳에서 실패 — 이 브랜치 diff 밖, main-base에도 같은 줄 존재. 이 job이 건드린 .cs 4개는 csharpier check 통과).
- `./gradlew --no-daemon test contractTest --continue`: **2510 중 3 실패 — 모두 main #1805가 새로 넣은 시험이고 판단이 필요해 멈춤(수정 커밋 없음).**
  - ZLinkJavaRawSpotNodeM6BTest.remoteLogicalMulticastPostsEachChildWithItsOwnReceivePermit, …AcceptsAdmittedSourceWithZeroWeight: "child 1 was not posted with a permit"(5s 대기 초과)
  - ZLinkRemoteMulticastPermitTest.remoteMulticastReachesEverySubscriberOnAHostWithOnePermit: "subscribers that received the multicast: 1 of 3"
  - main-base 4bf652482f에는 이 시험들이 없어 비교할 수 없다(main 9fb223eb3b에서는 PR 통과 기준으로 green이라고 본다 — 직접 돌리지 않음).
  - 원인(추정, 코드 읽기): #1805는 permit 하나로 fanout child 하나를 post하고, 다음 permit은 `acquireOrResume(pump, …)`가 **pump executor에 resume 작업을 넣어** 받는다.
    main에서는 pump가 `ingressWaitTimeout`(≤ liveness probe 간격) 안에 wait에서 돌아와 그 작업을 실행한다. 브랜치(c7228797d7·7787607857)는 polling tick을 없애고
    liveness deadline(peer가 없으면 `NO_MANAGEMENT_DEADLINE` = 무기한)까지 기다리며, Java port에는 permit 승인으로 pump wait를 깨우는 수단이 없다.
    socket에 남은 record가 없는 fanout 대기에서는 아무것도 wait를 깨우지 않아 resume이 실행되지 못한다.
  - 판단 필요: (a) permit 승인 시 pump wait를 깨우는 owner wake를 Java port에 두기(.NET #1769 `ReceiveWakeInterval` one-shot timer와 같은 방식), (b) fanout 대기 중에는 bounded wait로 되돌리기(브랜치의 deadline wait 원칙과 충돌), 등.
    스펙(application-job-queue §3 permit 대기 wake 책임)과 교차언어 대조가 필요한 runtime 결정이라 이 job에서 구현하지 않았다.
- 로그: /tmp/claude-0/-root/3a9c19c5-bb10-46f5-98c2-cf9785e10af4/scratchpad/br/java2.log

## 감독자 결정 반영 (#1648 후속)

### 항목 3 — jspre 복원
`git checkout --`으로 복원(커밋 없음). Java 시험 중(16:26) 다시 생성돼 마지막에 한 번 더 복원 — 현재 작업 트리 clean.

### 항목 2 — G1·G2 되돌림: 커밋 e7a0f2c5ba
`revert(java): restore main's serial queue drain executor and its tests`
- origin/main(9fb223eb3b) 상태로 되돌린 main 파일: `execution/ZLinkSerialExecutionQueue.java`, `runtime/actors/ZLinkActorDispatchSerials.java`
  (fc4a6c67ab의 지역 변수 개명도 이 되돌림으로 사라짐 — 파일이 main과 같아짐).
- 딸린 시험(브랜치 c7228797d7이 `Runnable::run` → `ForkJoinPool.commonPool()`로 바꾼 fixture) 15개를 main으로 되돌림:
  execution/ZLinkSerialExecutionQueueTest, actors/ZLinkActorDispatchSerialsTest, channels/{BlockingSubmission, HandlerFilterContract, MessageDispatcher, ReceiveLoopsApplicationJobQueue, ChannelRuntime, ClientServerMetadata}Test,
  internal/dispatch/ZLinkApplicationJobQueueExecutionBoundaryTest, spots/{DefaultSpotContext, SpotDirectPublish, SpotPublisherRuntime, SpotSerialExecutorAsync, SpotWideConsumerOwnership}Test.
  브랜치가 새로 넣은 execution/ZLinkReceiveWorkerTransitionTest(중간 executor 없음 검증)는 삭제.
- 남긴 것(receive path 소유): binding/ZLinkJavaRawMeshNode·ZLinkJavaRawServicePort·service/ZLinkServiceLivenessRegistry와 그 시험(M6A, ManagementWait, ShutdownSeal).
- 확인: `git diff origin/main -- <ZLinkSerialExecutionQueue.java> <ZLinkActorDispatchSerials.java>` 출력 0줄. Java 전체 컴파일 성공.

### 항목 1 — permit 반환 시 pump wake: **구현하지 않고 멈춤(판단 필요)**
지시는 "새 wake 수단·timer·상태를 만들지 말고 pump가 이미 쓰는 poller wake(signalStateChanged 등)를 permit 반환 지점에서 부른다"였으나,
Java raw mesh pump에는 **poller wait를 깨우는 기존 경로가 없다**.
- pump wait: `ZLinkJavaRawMeshNode.pumpTurn`(binding/ZLinkJavaRawMeshNode.java:4550) → `ZLinkJavaSocketReceivePoller.waitForReady`(:100 `poller.wait`). 이 poller에는 ROUTER socket 한 slot만 등록된다(ZLinkJavaSocketReceivePoller.java:69/77/84). timer·fd 등 다른 source가 없다.
- `signalStateChanged`(ZLinkJavaRawMeshNode.java:1043) → `deliverStateSignal`(:1056)은 descriptor state listener를 비동기로 부를 뿐 poller를 깨우지 않는다.
- permit 반환: `ZLinkApplicationJobQueue.acquireOrResume`(internal/dispatch/ZLinkApplicationJobQueue.java:256)가 대기 permit 승인 때 `executor.execute(resume)`(:276) — executor가 pump 단일 스레드라, pump가 `poller.wait`에 막혀 있으면 resume 작업이 실행되지 못한다(pump 호출 지점 ZLinkJavaRawMeshNode.java:4592).
- Java의 다른 receive owner(channels/ZLinkChannelReceiveLoops.java:22)는 `RECEIVE_POLL_TIMEOUT = 250ms` 고정 대기로 이 문제를 가린다(I0와 같은 유형).
- 교차언어 대조 — 세 언어 모두 permit 승인 시 수신 owner를 명시적으로 깨우며, 그 수단이 .NET·C++에서는 receive poller의 one-shot timer다:
  - .NET: `WaitForRawApplicationAdmissionAsync`(dotnet/.../Runtime/Service/ZLinkManagedMeshNode.cs:5593) 승인 → `_reservedRawApplicationAdmission` 저장 → `SignalReceiveOwnerUnderLock()`(:5610, 정의 :5402) = `_receiveWakeTimer.Start(1 tick, 1)`; timer는 :427에서 만들어 receive poller slot 2(`ReceiveWakePollerSlot`)에 등록.
  - C++: `application_job_queue_t::release_permit`(cpp/framework/src/runtime/dispatch/application_job_queue.hpp:798) → waiter callback(:829, supply slot :974-982) → `state->wake()` = `signal_dispatch_activity`(stateful/public_host_runtime.cpp:6566) → `raw_mesh_node_owner_t::signal_activity`(mesh/raw_mesh_node_owner.cpp:3441-3452) → `raw_route_port_t::signal_activity` → `_wake_timer.signal()`(backend/raw_route_port.cpp:395-398; `runtime_wake_timer_t` eventing/runtime_wake_timer.hpp:21-80, poller 등록 raw_route_port.cpp:133, one-shot `start(1ns,1)`). 수신 thread는 -1(무기한)+deadline으로 대기(mesh/mesh_node_host_service.cpp:2805-2808).
  - Node: blocking poll이 없다. 승인 → `ApplicationJobQueue.release`/`drainWaiters`(packages/framework/src/runtime/host/application-job-queue.ts:498-505, :537-546) → resume(foundation/raw-service-mesh-runtime.ts:736-746)가 `receivePermit` 저장 후 `onReceiveReady()`(:743) → `onReadable(true,false)`(backend/node/node-raw-mesh-backend.ts:395, :189-193) → `pump()`(:1561-1579). 1ms maintenance timer(:150)는 수신하지 않는다.
- 결론: Java에서 이 결정을 따르려면 .NET·C++과 같은 receive poller one-shot `ZlinkTimer`(binding `Poller.add(ZlinkTimer, slot)` 존재 — bindings/java/.../contracts/eventing/Poller.java:21)를 **새로** 두어야 한다. 이는 지시의 "새 wake 수단·timer를 만들지 않는다"와 충돌한다.
  선택지: (a) .NET·C++과 같은 one-shot timer wake를 Java poller에 추가(규칙 수는 네 언어가 같은 수단으로 맞춰짐), (b) 다른 기존 경로가 있다면 감독자가 지정. 결정을 기다린다.

### 항목 2 뒤 Java 시험 (`./gradlew --no-daemon test contractTest --continue`)
- java3: `ZLinkJavaRawMeshNodeM6ATest.command44UsesOneWayInfrastructureDispatch`가 `NativeContext.close`→`ctxTerm`에서 26분 정지(남은 pump thread 없음, jstack: scratchpad/br/java3-hang-jstack.txt). 그 test JVM pid만 kill. 같은 class를 따로 7회 돌리면 모두 통과 → 간헐.
- java4(재실행): 2509 중 2 실패 — `ZLinkRemoteMulticastPermitTest.remoteMulticastReachesEverySubscriberOnAHostWithOnePermit`(항목 1 원인),
  `ZLinkCanonicalDirectJoinHostIntegrationTest … LEAVE_THROW`(ZlinkCloseException at NativeSocketRuntime.java:621).
- 선택 재실행 3회(Join + M6B + RemoteMulticastPermit): Join 3/3 통과; #1805 시험은 매회 2~3건 실패(M6B 중 ZeroWeight·BeforeDescriptorUpdate·PostsEachChild가 번갈아) — permit resume이 다른 wake에 기대는 timing 의존, 항목 1과 같은 원인.
- main-base 4bf652482f에서 Join class 3/3 통과(#1805 시험은 main-base에 없음). Join LEAVE_THROW는 branch 4회 중 1회 실패 — 새 flake 후보로 남김(항목 1 수정 뒤 재측정 필요).

## 항목 1(a) 구현 — permit 반환 시 Java 수신 owner one-shot wake: 커밋 9f1623f484
`fix(java): wake the raw receive owner when a permit returns` (본문 Refs #1648, Co-Authored-By)
- `ZLinkJavaSocketReceivePoller`: route observer(= socket의 유일한 receive owner, `observesRoutes`)일 때만 `ZlinkTimer` 하나를 만들어 **기존 poller**의 slot 2(`OWNER_WAKE_SLOT`)에 등록(두 번째 poller 없음).
  `wakeOwner()`가 `ownerWakeArmed.compareAndSet(false,true)` 한 곳에서 판정하고 `start(1ns, 1)`(one-shot, 주기 아님). 이미 걸려 있으면 다시 걸지 않는다.
  `waitForReady`에서 timer event면 `recv()` 뒤 armed 해제(resume 작업은 wake 전에 이미 pump queue에 있으므로 해제 순서로 wake를 잃지 않는다). close는 timer monitor 안에서 closed 표시 후 timer close.
- `ZLinkJavaRawServicePort.receiveOwnerWake(router)`: 그 poller의 `wakeOwner`를 돌려준다.
- `ZLinkJavaRawMeshNode.startPump`: `pumpResume = cmd -> { pump.execute(cmd); wake.run(); }`; `drainIngressBatch`의 `acquireOrResume` executor를 `pump` → `pumpResume`으로. 즉 capacity waiter 재개(permit 반환) 때만 한 번 wake.
- 교차언어: .NET `WaitForRawApplicationAdmissionAsync`→`SignalReceiveOwnerUnderLock`(ZLinkManagedMeshNode.cs:5610, timer :427), C++ `raw_route_port_t::signal_activity`→`_wake_timer.signal()`(raw_route_port.cpp:395)와 같은 수단. Node는 poller 없이 `onReceiveReady()` 직접 호출.

### 결과
- 수정 전(e7a0f2c5ba): `--tests M6B --tests RemoteMulticastPermit` 실제 실행 2회(sel1, sel3) 모두 실패(RemoteMulticastPermit 매회 + M6B 1~2건).
- 수정 후: 같은 선택 `--rerun` 5회 모두 통과(M6B 42 + RemoteMulticastPermit 1, UP-TO-DATE 없음). 로그 scratchpad/rep-mc/.
- Java `./gradlew --no-daemon test contractTest --continue` 전체: **BUILD SUCCESSFUL, 실패 0**(java5.log). → main 대비 새 실패 0.
- 정정: 앞 절의 "M6A class 7/7 통과", "main-base Join 3/3"은 Gradle `test` UP-TO-DATE로 실제 실행이 각 1~2회뿐이었다(m6a2만 실행, base-join1만 실행). 이후 반복은 모두 `--rerun`으로 했다.

## CanonicalDirectJoin 20회 비교 (`:zlink-framework-core:test --rerun --tests '*ZLinkCanonicalDirectJoinHostIntegrationTest'`, 9 scenario/회)
| | 실패 |
|---|---|
| branch 9f1623f484 | **1/20** (run9, scenario JOINED_GENERIC_FAILURE) — 앞서 전체 실행 java4에서 LEAVE_THROW 1건도 같은 서명 |
| main-base 4bf652482f | 0/20 |

**결함(브랜치 원인)으로 판정, 수정하지 않고 보고.** 실패 한 건의 기록(scratchpad/rep-join-br/xml9, run9-stderr.txt):
- `ZLinkFrameworkShutdown$Failure` ← `ZlinkCloseException at NativeSocketRuntime.closeInternal:621` ← `NativeRouterSocket.close` ← `ZLinkJavaRawServicePort.close:441` ← `ZLinkJavaRawMeshNode.close`(:4510) ← `ZLinkJavaRawSpotNode.close:1144`, 이어 host shutdown deadline 초과.
- Core 규칙: `zlink_close`는 같은 handle에서 다른 thread가 admitted API를 실행 중이면 `EBUSY`로 실패한다(core/doc/spec/core/socket/README.en.md:62, :697-699; `ZLINK_CLOSE_BUSY`).
- 원인: 브랜치 c7228797d7이 `ZLinkJavaRawMeshNode.close`(:4484) 순서를 main의 "pump `shutdownNow` → **pump 종료 대기** → … → `port.close()`"에서
  "pump `shutdownNow` → … → `port.close()` → pump 종료 대기"로 바꿨다(무기한 wait를 socket close의 POLLERR로 깨우려는 목적). 그래서 pump thread가 진행 중인 회전에서
  socket API(recv·route snapshot·liveness send)를 실행하는 순간에 router close가 겹치면 EBUSY가 난다. main은 종료 대기 뒤 close라 겹치지 않는다(0/20).
  java3의 M6A `ctxTerm` 26분 정지(pump thread 없음, 닫히지 않은 socket이 남은 상태)도 같은 원인으로 보인다.
- message-flow trace: 이 실패는 dispatch flow가 아니라 close 경로라 flow trace를 켠 재현은 하지 않았다. 판단 근거는 test XML의 전체 stack과 JUL stderr 로그다(필요하면 `options.configureDispatch().messageFlow(NORMAL)`로 켜서 다시 잰다).
- 제안(감독자 판단 필요): close에서 wait를 socket close 대신 방금 넣은 owner wake로 깨우고(.NET `DisposeCoreAsync`가 stop 때 `SignalReceiveOwner()` 하는 것과 같음, ZLinkManagedMeshNode.cs:3236), main 순서대로 pump 종료 대기 뒤 `port.close()`.

## 종료 순서 수정: 커밋 e75eaa12d8 `fix(java): stop the raw mesh pump before closing its ROUTER`
- 재현 근거(수정 전 9f1623f484): CanonicalDirectJoin `--rerun` 20회 중 1회 실패(run9, JOINED_GENERIC_FAILURE) — `ZlinkCloseException` at `NativeSocketRuntime.closeInternal:621` ← `ZLinkJavaRawServicePort.close:441` ← `ZLinkJavaRawMeshNode.close`; main-base 0/20.
- 수정: `close()`에서 `pump.shutdownNow()` → `pumpWake.run()`(새 owner wake로 pump wait 깨움) → pump 종료 대기 → 미완료 fanout 정리 → … → `port.close()`(main 순서). `pumpWake`는 `startPump`에서 `port.receiveOwnerWake(pumpSocket)`로 한 번 받는다. .NET `DisposeCoreAsync`의 `SignalReceiveOwner()`(ZLinkManagedMeshNode.cs:3236)와 같은 방식.
- 결과(e75eaa12d8, merge 전):
  - CanonicalDirectJoin `--rerun` 40회: **branch 0/40 실패**, main-base(4bf652482f→ 같은 worktree) 0/40.
  - M6A `--rerun` 20회: 0/20 실패(ctxTerm 정지 재발 없음 — java3의 정지와 같은 원인으로 판단: close가 EBUSY로 거부되면 ROUTER가 열린 채 남아 ctx term이 기다린다).
  - Java `test contractTest --continue --rerun-tasks` 전체(ZLINK_REDIS_LOCATION_ENDPOINT=redis://127.0.0.1:6379, Redis 가동): **3062 tests, 실패 0, skip 0**.

## main d2489ac50f 재merge: 커밋 6f8a338499 (부모 e75eaa12d8, d2489ac50f; 본문 Refs #1648, Co-Authored-By)
파일별 해소:
- framework/languages/java/.../binding/ZLinkJavaRawMeshNode.java (충돌 1 hunk, close): main #1810(47cbb64464)이 `awaitExecutorTermination`을 지우고 `currentPump.close()`로 "port를 닫기 전에 pump join"을 했다 — 이 브랜치 수정과 같은 방향(pump 정지→대기→close).
  최종: `shutdownNow()` → `pumpWake.run()` → `currentPump.close()`. wake는 유지(브랜치 pump는 무기한 deadline wait라 깨우지 않으면 `close()`가 끝나지 않는다).
- framework/languages/dotnet/.../ZLinkManagedMeshNode.cs: 자동 merge(main #1807 relocation stage trace·timeout 상수 등). 브랜치의 receive reservation·readiness 부분과 겹치지 않음. 빌드·시험 통과.
- 나머지 40커밋(#1811 binding slot, #1812, #1814 C++, Java binding VERSION 1.19.1 등)은 브랜치와 겹치는 파일 없음. Java는 Maven Central 배포 binding 1.19.1(libs.versions.toml `zlinkBindings = "1.19.1"`)로 빌드. Core 변경 없음 → rebuild-dev 생략.

### merge 뒤 시험
- .NET UnitTests(main filter) 통과, join filter 통과, ContractTests 통과(scratchpad/m3/dn-*.log).
- CanonicalDirectJoin `--rerun` 20회: 0/20 실패.
- Java `test contractTest` 전체: **zlink-framework-core:test가 정지** — main #1810이 새로 넣은 `ZLinkJoinedActorSendAdmissionTest.joinedCallbackSendToJoiningActorIsAdmittedBeforeTheJoinCompletes`(crossNode=true)에서 `ZLinkFrameworkRuntime.close`가 끝나지 않는다. 다른 모듈(redis 포함)은 통과.
  - 그 class만 `--rerun` 반복(120~240s 넘으면 jstack 후 그 test JVM pid만 kill): branch 정지 4/5, 1/2, 0/3, 0/6(--info), 2/8, 1/10; **main d2489ac50f(zlink-main-base, 지금 이 commit) 0/5**.
  - 정지 위치(임시 stage 기록): 두 번째(source) host close의 `instance_close`(`actors::closeAsync`) 시작 후 끝나지 않음. #1810 이후 `deferCloseStage`에는 deadline이 없다(ZLinkFrameworkShutdown).
  - message-flow trace(DETAILED, JUL을 파일로 — 임시 계측, 조사 뒤 삭제; diff는 scratchpad/m3/temp-instrumentation.diff): 정지한 run의 마지막 crossNode scenario에서 source Actor의 `JoinSpot`이 `phase=sent` 뒤 **`reply_received`가 없다**(앞 scenario들은 있음; 한 정상 scenario는 reply가 280ms 늦게 commonPool thread에서 도착). 즉 source가 target의 JoinSpot reply를 받지 못해 Actor close가 pending join을 기다린다.
  - 실험: pump wait를 매 회전 100ms로 고정(임시)해도 2/8 정지 → 단순한 poll tick 부재만은 아니다. 남은 브랜치 Java 차이(RawMeshNode의 deadline wait·permit wake·intent close 직접 disconnect(`finishRequestedIntentCloses`)·liveness registry)를 좁혀야 한다.
  - **수정하지 않고 멈춤 — 원인 미확정, 감독자 판단 필요.** 임시 계측은 모두 되돌렸다(작업 트리 clean).

## ZLinkJoinedActorSendAdmissionTest crossNode 정지 — 기준선과 bisect
시험: `:zlink-framework-core:test --rerun --tests '*ZLinkJoinedActorSendAdmissionTest.joinedCallbackSendToJoiningActorIsAdmittedBeforeTheJoinCompletes'`(4 parameter, crossNode 2개 포함 — Gradle filter로 parameter 하나만 고를 수 없어 method 단위). 150s 넘으면 jstack 후 그 test JVM pid만 kill. 같은 시간대에 다른 job(cpp 빌드, m1673h gradle)이 돌았다.

### 1. 기준선(각 30회)
| 상태 | 정지 |
|---|---|
| main adfdff424a (zlink-main-base를 `checkout --detach`로 이동; d2489ac50f→adfdff424a 사이 Java 변경 없음) | **0/30** |
| 브랜치 6f8a338499 | **10/30** (run 1-5, 19, 20, 22, 24, 27) |
→ main 0/30이고 브랜치만 재현 → 브랜치 결함으로 보고 bisect.

### main 55f3dc90cf(#1827) merge: 커밋 089baca8e6 (Refs #1648, Co-Authored-By)
- 충돌 없음. 겹친 파일(자동 merge): dotnet `ZLinkManagedMeshNode.cs`, `MeshPermitHandoffTests.cs`, `ServiceRuntimeFoundationTests.cs`. Java는 겹치는 파일 없음, Core·binding 변경 없음.
- merge 뒤에도 정지 재현: 12회 중 5회(중단). → .NET 빌드·시험은 이 merge 뒤 아직 돌리지 않았다.

### 2. bisect
브랜치 커밋을 그대로 checkout하면 이 시험(main #1810)이 없어서 git bisect를 쓸 수 없다. 대신 HEAD(089baca8e6) 위에 브랜치 Java 파일 4개(RawMeshNode·RawServicePort·SocketReceivePoller·LivenessRegistry)와 그 시험 3개를 각 브랜치 커밋 상태로 놓고 20회씩 돌렸다(1회라도 정지하면 bad). main은 9fb223eb3b 이후 이 4개 파일을 #1810 close 한 곳만 바꿨다. 실험이 끝난 뒤 HEAD로 되돌렸다.
| 상태 | 결과 |
|---|---|
| S1 = e7a0f2c5ba(deadline wait, intent close 직접 disconnect, permit wake 없음) | **good 0/20** |
| S2 = 9f1623f484(permit wake 추가) | bad (run4) |
| S2a = 9f1623f484인데 `acquireOrResume` executor를 `pump`로(timer는 등록하지만 한 번도 start하지 않음) | bad (run6) |
| S2b = S1 + timer 등록만(Poller·ServicePort @9f1623f484, RawMeshNode @e7a0f2c5ba) | bad (run1, 모양이 다름: 아래) |
| E1 = HEAD + intent close를 main처럼 pump queue로 되돌림 | bad (run1) — intent close 변경은 원인이 아님 |
→ 원인 커밋: **9f1623f484**. 그 안에서도 wake 호출이 아니라 **수신 poller에 timer source를 등록하는 변경**(`ZLinkJavaSocketReceivePoller.java:60` `ownerWake = observesRoutes ? Zlink.createTimer() : null`, `:84` `poller.add(ownerWake, OWNER_WAKE_SLOT)`, `:35` `PollEvents(2)`, `:112-121` timer event 처리)만 있어도 정지가 난다.

### 정정과 관찰
- 앞 절의 "JoinSpot reply 유실"은 틀렸다: 정지한 run에서도 이 시험의 assertion(`onJoinCompleted`가 남기는 "completed" 포함)은 통과했다. reply는 도착했고, flow trace에서 그 scenario의 `reply_received` 줄이 빠졌던 것뿐이다. 정지는 **host close**에서 난다.
- 정지 모양은 두 가지다(test 줄로 구분):
  - (a) `:160`(source.close, try-with-resources 끝). mesh pump thread 1개, 임시 stage 기록에서 `instance_close`(`actors::closeAsync`, #1810 이후 deadline 없는 close stage)가 시작한 뒤 끝나지 않음. HEAD·S2·S2a·E1이 이 모양.
  - (b) `:158`(target.close). pump thread 2개. S2b와 HEAD 1건.
- 1개만 남은 pump는 poller wait 중이다. (a)에서 두 runtime이 아직 모두 살아 있어야 할 때 pump가 1개뿐인 이유(pump 종료인지 다른 runtime의 정상 종료인지)는 확인하지 못했다. 임시 pump 종료 기록 1회에서는 예외 종료 흔적이 없었다.
- 한 번도 start하지 않은 timer가 동작을 바꾸는 경로 후보(확인 전): binding `NativePoller.wait`가 socket이 아닌 source가 있으면 다른 결과 경로를 탄다(bindings/java/.../eventing/NativePoller.java:283-299, `containsOnlySockets=false`). Core poller의 "등록이 전부 닫힌 socket이면 기다리지 않는다" 규칙(core/doc/spec/core/05-polling.en.md:307, :386)이 열린 timer 때문에 더는 성립하지 않아, close 때 POLLERR 한 번 뒤의 wait 동작이 달라진다((b) 모양과 맞음).
- 수정안(적용하지 않음, 확인 요청):
  - (가) 원인을 더 좁힌다 — 같은 수단으로 Java binding poller가 timer source를 섞었을 때의 wait·close 동작을 단독 repro로 확인한다. binding 결함이면 binding Issue로 보고한다.
  - (나) owner wake를 수신 poller의 timer가 아니라 다른 기존 수단으로 낸다(감독자 결정 1의 "기존 poller에 등록" 조건과 충돌하므로 판단 필요).
- 임시 계측(stage·flow·pump 종료 기록)과 실험 변경(E1, S1~S2b, 100ms 고정)은 모두 되돌렸다. 작업 트리 clean, HEAD 089baca8e6. 로그: scratchpad/b30-*, s1, s2, s2a, s2b, e1, pd.

## (a) binding 수준 단독 재현 — 가설과 일부 다름, 멈춤
임시 JUnit(커밋하지 않음, 사본 scratchpad/TmpG1648PollerTimerCloseRepro.java): context·ROUTER(inproc bind), `Zlink.createPoller()`에 socket(POLLIN) 등록, timer 있음/없음. 별도 platform thread가 `poller.wait(events(2), -1)`, latch 2초 상한, `router.close()`. timer가 있는 경우에만 마지막에 timer를 start해 waiter를 풀었다.
| | 1번째 wait(close 중) | close 뒤 2번째 wait(-1) |
|---|---|---|
| timer 없음 | 반환, count=1 (POLLERR) | **즉시 반환, count=0** |
| 시작 안 한 timer 등록 | 반환, count=1 (POLLERR) | **2초 안에 반환하지 않음** (timer를 start하자 풀림) |
- 가설 중 "socket close가 들어와도 wait(-1)가 돌아오지 않는다"는 **틀렸다**. close는 timer가 있어도 POLLERR 한 번으로 진행 중인 wait를 깨운다.
- 맞는 부분: 그 POLLERR를 한 번 받은 **뒤의** wait는 timer가 있으면 무한 대기한다. timer가 없으면 Core 05-polling :307·:386("등록이 모두 닫힌 socket이면 기다리지 않음")대로 즉시 0을 반환한다. Core 계약대로의 동작이고 binding 결함은 아니다.
- 이 결과만으로는 정지 모양 (a)를 설명하지 못한다. (a)는 source host close의 `instance_close` 단계에서 멈추는데, 이 단계는 source mesh의 ROUTER를 닫기 전(`mesh_nodes_close` 앞)이다. 이 때 source pump의 poller에는 닫힌 socket이 없다. target ROUTER는 별도 poller다.
- 모양 (b)(target.close :158, pump 2개)는 "POLLERR를 먼저 받은 뒤 다시 wait" 경로로 설명될 수 있다(예: drain loop의 `waitForReadable(ZERO)`가 POLLERR를 받고, 다음 회전의 무기한 wait가 멈춤).
- 지시에 따라 수정(2)과 검증(3)은 하지 않았다. 결정이 필요하다: (b)만 이 규칙으로 고칠지, (a)를 위해 instance_close가 기다리는 대상을 먼저 계측할지.
- .NET build·unit·contract(#1827 merge 뒤)도 이 판단 뒤로 미뤘다.

## 정지율 재측정, :160 계측, main merge 2회
### 사고 기록(공유 머신)
- 이전 watcher(`hangrep.sh`)가 `pgrep -f 'java.*Gradle Test Executor'`로 대상을 골라서, 150초를 넘은 **다른 job(Java 2차)의 test JVM까지 kill했다**(최소 2건: 요약에서 `rc=0 hung=1`인 run). 앞 절의 정지 표에서 rc=0·hung=1 줄은 다른 job의 JVM이다. 감독자 지시로 이 watcher를 폐기했다.
- 새 watcher(`hangrep2.sh`)는 자기가 띄운 gradle의 ppid 트리만 본다. 120초가 지나면 jstack과 진단 로그만 수집하고 kill하지 않는다. 시험은 JUnit 기본 timeout 150s(`JAVA_TOOL_OPTIONS=-Djunit.jupiter.execution.timeout.default=150s`)로 끝난다. 자기 트리를 정리하는 것은 test JVM이 400초를 넘을 때뿐이다.
- 그 사이 남은 내 고아 gradle 3개(그 중 1개가 jvm gate lock을 35분 잡고 있었다)는 cwd를 확인한 뒤 내 트리만 정리했다.

### merge
| 커밋 | main | 충돌 | 비고 |
|---|---|---|---|
| 3b28173c7a | 9450eff5ed (#1828 등) | 없음 | 자동 merge: Java `ZLinkJavaRawMeshNode.java`, `ZLinkJavaRawMeshNodeM6ATest.java` |
| c35df7c2a1 | 5994ffa9d7 (#1847 등) | 없음 | 겹치는 파일 없음. .NET binding 1.19.1 → rebuild-dev 다시 실행(Zlink.1.19.1.nupkg) |
두 merge 모두 본문 `Refs #1648`, 마지막 줄 Co-Authored-By.

### crossNode 정지율(`hangrep2`, method 단독 30회, 4 parameter)
| 상태 | 정지 | 모양 |
|---|---|---|
| 브랜치 3b28173c7a (+#1828) | 1/30 | :160 |
| 브랜치 c35df7c2a1 (+#1847) | **13/30** | 모두 :160 (`[4] true, SPOT_WIDE`) |
| main 5994ffa9d7 (zlink-main-base를 이 commit으로 이동) | **0/30** | — |
- **:158은 #1847 뒤 재현되지 않았다**(c35df7c2a1 30회 중 0). 그래서 :158의 POLLERR 뒤 무한 wait 경로는 file:line으로 확정하지 못했다(재현 없음).

### :160 계측 결과(임시 계측, 조사 뒤 모두 되돌림; diff는 scratchpad/diag3.diff)
계측 내용: host close stage, Actor close·teardown 단계, deferred Join barrier, ACTOR_LEFT 수신·완료, pump·poller·queue 상태, close 20초 watchdog의 전체 thread 덤프.
정지한 run 두 번(dg4 run7, dg5 run1)의 공통 순서:
1. source mesh pump가 target에서 온 **ACTOR_LEFT(command 29)**를 받는다(`ZLinkJavaRawMeshNode.dispatchActorLeft` :5273, handler 호출 :5290). 이것은 target host close가 player-1 Actor를 닫는 시점과 겹친다.
2. 그 전에 source player-1의 **deferred Join barrier**가 Actor lifecycle lane에서 실행을 시작했고 끝나지 않는다(`ZLinkActorRuntime.submitDeferredJoinBarrier` :4228 → `ZLinkActorDispatchSerials.enqueueBarrier` :430, `executeLifecycleNext(() -> runTurn(...))` :447; 계측 "enqueue-barrier-451-run"은 있고 "-done"은 없다).
3. source host close의 `instance_close`가 `closeActorEntry`(:4749) → `beginTeardown`(:397, created=true) → `startTeardown`(:545)에서 sealing을 마치고 APPLICATION quiescence에 도달한다(:551). 그러나 `executeLifecycle(cleanup)`(:552)이 위 barrier 뒤의 lifecycle 대기열에 들어가 **cleanup이 시작되지 않는다**. watchdog의 lane 상태: `active=LIFECYCLE/ZLinkActorDispatchSerials$$Lambda … life=1 outstanding=2`.
4. ACTOR_LEFT 처리가 끝난 뒤 commonPool에서 같은 Actor의 `beginTeardown`이 한 번 더 불린다(created=false, 기존 teardown terminal을 기다림 — `completeRelocationSource` :4678 → :4708 경로로 보인다).
- 같은 시점의 다른 상태: 두 pump는 모두 살아서 liveness deadline으로 wait 중이다. application job queue는 `permitsInUse=0`, `capacityWaiters=0`, `pendingAcquire=false`이고 owner wake timer는 armed=false다.
  → **permit 소진이나 wake 유실이 아니다.** Actor lifecycle lane 안의 대기 순환으로 보인다: deferred Join barrier(활성) → (추정) Join 정산이 ACTOR_LEFT·source leave 완료를 기다림 → 그 완료가 teardown terminal을 기다림 → teardown cleanup은 barrier 뒤에 줄을 섬.
  barrier 연산이 정확히 무엇을 기다리는지(2→4를 잇는 고리)는 아직 계측하지 않았다.
- 브랜치의 몫: 대기 구조는 main 코드(Actor dispatch·Join adapter)에 있다. 브랜치는 수신 타이밍만 바꾼다(ACTOR_LEFT가 source close 직전 수 ms 안에 처리됨). 그런데도 main은 60회 중 0이고 브랜치는 30회 중 13이다. 브랜치의 어떤 타이밍 변화가 이 창을 넓히는지는 아직 모른다.

### 결정 소유 표(현재까지)
| 결정 | 지금 내리는 곳 | 비고 |
|---|---|---|
| Actor lifecycle lane의 순서(barrier·teardown) | `ZLinkActorDispatchSerials.enqueueBarrier` :430·:447, `beginTeardown` :397, `startTeardown` :545·:552 | 같은 lane에 들어간 barrier 연산이 teardown terminal을 (간접으로) 기다리면 순환 |
| deferred Join 실행 | `ZLinkActorRuntime.submitDeferredJoinBarrier` :4228 (호출 `ZLinkActorSpotJoinCall` :202, `ZLinkActorEntrySpotJoinCall` :83) | barrier 연산 = Join 전체 |
| ACTOR_LEFT 처리와 source 정리 | `ZLinkJavaRawMeshNode.dispatchActorLeft` :5273 → Join canonical adapter → `ZLinkActorRuntime.completeRelocationSource` :4678·:4708 | 기존 teardown을 기다림 |
| 수신 owner wake(브랜치) | `ZLinkJavaSocketReceivePoller.wakeOwner`, `ZLinkJavaRawMeshNode.pumpResume`·`close` | 이 정지에는 관여하지 않음(permit·pump 정상) |

### 수정안(적용하지 않음, 승인 필요)
- :160과 :158은 한 원인·한 규칙으로 모을 수 없다. :160은 Actor lifecycle lane의 대기 순환이고, :158(POLLERR 뒤 wait)은 #1847 뒤 재현되지 않는다.
- :160 다음 단계(제안): barrier 연산(`runTurn` 안의 Join)이 기다리는 future를 계측해 2→4 고리를 확정한다. 그 다음 소유자를 정한다 — host close의 Actor teardown이 진행 중인 lifecycle barrier를 기다리지 않게 할지(Join barrier를 teardown 전에 정산할지), 아니면 ACTOR_LEFT 경로가 teardown terminal을 기다리지 않게 할지. 이 순환이 main에도 있는 구조이므로 수정은 main 소유 결함으로 따로 다루는 편이 맞아 보인다.

### .NET (c35df7c2a1, rebuild-dev 뒤 Zlink.1.19.1.nupkg)
- build 성공. ContractTests 96/96. UnitTests join filter 24/24.
- UnitTests main filter: 2988 중 2 실패 — `MessagingHotPathWireTests.BorrowedPayloadPreservesAllPartRangesAndOwnedMemory(count: 1|2)`, `ObjectDisposedException: Message`(ZLinkApplicationPayloadEnvelopeCodec.cs:145).
  main-base 5994ffa9d7(rebuild-dev로 1.19.1 nupkg 생성, 소스 변경 없음)에서도 같은 2건이 실패한다 → **기존 결함(main #1841 .NET binding 1.19.1 Message 소유 변경 이후로 보임), 새 실패 0**. 로그 scratchpad/dn2/.
- 작업 트리 clean(HEAD c35df7c2a1), 임시 계측 전부 되돌림, push 없음.

## main 3970d1f91b merge와 :160 재측정 — 성능 측정을 위해 일시 정지
### merge 커밋 57547d0b8c (부모 c35df7c2a1, 3970d1f91b; Refs #1648, Co-Authored-By)
- 충돌 파일 1개: framework/languages/java/.../binding/ZLinkJavaRawMeshNode.java (hunk 2개).
  - main #1852(#1761, MeshNode §7.1 "ADMIT를 받기 전에는 선택된 route에 기존 liveness 관리 주기마다 HELLO를 다시 보낸다. 별도 timer를 두지 않는다", 03-mesh-node.en.md:295-300)가 `announceExpectedPeers`를 `tickLiveness` 안으로 옮겼다. main은 이를 pump의 고정 100ms 대기(`ingressWaitTimeout`)마다 실행해 재전송 주기를 얻는다.
  - 브랜치는 그 고정 대기를 없애고 liveness deadline까지 기다린다. admission 전에는 liveness에 등록된 peer가 없어 deadline이 무기한이므로, 그대로 합치면 §7.1 재전송이 일어나지 않는다.
  - 해소: `pumpTurn`은 브랜치 형태(`finishRequestedIntentCloses()`만 호출). `tickLiveness`는 main처럼 먼저 `announceExpectedPeers()`를 부르고 다음 대기 시간을 돌려준다. **추가 규칙(검토 필요)**: ADMIT가 아직 없는 선택된 route가 있으면(`awaitingAdmit()` — `helloExchanges` 항목이 없거나 submission이 진행 중) 다음 대기를 liveness probe 간격(`helloRetryInterval = livenessProbeInterval`) 이하로 자른다(`earlier`). 새 timer는 없고, 판정 위치는 `tickLiveness` 한 곳이다.
  - 나머지(dotnet `ZLinkManagedMeshNode.cs`, `ServiceRuntimeFoundationTests.cs`, Java M6A·ShutdownSeal 시험)는 자동 merge.
- 검증: Java 컴파일, `--rerun --tests '*ZLinkJavaRawMeshNode*' '*ZLinkJavaRawSpotNode*' '*ZLinkRemoteMulticastPermitTest'` 통과. 전체 contractTest와 .NET은 아직 돌리지 않았다.

### crossNode 정지율(57547d0b8c)
- 3회 연속 정지(모두 :160, `[4] true, SPOT_WIDE`, pump 1개). 4회째 도중 감독자의 일시 정지 요청으로 중단했다(새 실행 없음, 진행 중이던 내 Gradle이 끝나는 것을 확인함). main 3970d1f91b 기준선은 아직 측정하지 않았다.

### 남은 순서(재개 뒤)
1. crossNode 30회를 마저 측정한다(브랜치 57547d0b8c, main-base를 최신 main으로 옮겨 30회).
2. Join barrier가 무엇을 기다리는지 계측해 고리 1→3을 확정한다(`ZLinkActorDispatchSerials` :447 runTurn 안의 Join future, `completeRelocationSource` 경로의 thread와 대기 대상).
3. main-base에서 gate·barrier로 "진행 중 deferred Join barrier + 종료 teardown + ACTOR_LEFT 수신"을 강제하는 결정적 재현을 만든다(sleep 없음).
4. 결정 소유 표와 수정 선택지를 스펙 조항과 대조해 보고하고 멈춘다.
- 작업 트리 clean(HEAD 57547d0b8c), 임시 계측 없음, push 없음.

## :160 정지 — main 결함 판정용 측정·계측·재현 (재개 뒤)
### ① 정지율(`hangrep2`, 자기 Gradle 트리만, kill 없음, method 단독 30회)
| 상태 | 정지 | 모양 |
|---|---|---|
| main b70ed64be5 (zlink-main-base를 이 commit으로 이동) | **0/30** | — |
| 브랜치 57547d0b8c | **16/30** | 모두 :160 (`[4] true, SPOT_WIDE`) |
로그: scratchpad/r1-base, r1-br.

### ② 계측으로 확정한 고리(임시 계측, 되돌림; diff scratchpad/diag5.diff, 정지 run의 기록 scratchpad/dg6·dg7)
정지한 run(dg7 run4)의 순서. 시각은 nanoTime, rt는 runtime identity.
1. 시험 본문 동안 source player-1의 deferred Join barrier turn이 lifecycle lane에서 실행 중이다(`enqueue-barrier-451-run`, `join-deferred-execute-start`; `ZLinkActorDispatchSerials.enqueueBarrier` :430 → `executeLifecycleNext(() -> runTurn(...))` :447, operation = `ZLinkActorSpotJoinCall.executeDeferred` :298 → `bounded.execute` :321 → canonical `relocate` → `executeSource` → `ZLinkRelocationHandOff.run` → `settleSource`). `join-deferred-execute-done`은 끝까지 없다.
2. target host가 닫히면서 source 정산이 **`settleSource(SOURCE_LEASE_EXPIRED)`**(#1847 expired-owner terminal, `ZLinkActorJoinCanonicalAdapter.java:148`)로 끝난다. 이 처리는 commonPool thread에서 돈다(…394489).
3. 2ms 뒤 source host close가 `closeActorEntry`(ZLinkActorRuntime.java:4749) → `beginTeardown`(ZLinkActorDispatchSerials.java:397, created=true) → `startTeardown` :545에서 seal을 마치고 quiescence에 도달한다(:551). 이어지는 `executeLifecycle(cleanup)` :552는 **1의 barrier turn 뒤 lifecycle 대기열**에 들어가 시작되지 않는다(watchdog의 lane 상태: active=LIFECYCLE barrier, life=1).
4. 2의 연장으로 `discardAfterSourceLeaseExpiry`(ZLinkStandaloneActorRelocationSourceBuilder.java:1417, :1446-1448) → `cleanupLocal` :1388/:1401/:1409 → `ZLinkActorSessionCoordinator.completeRelocationSource` :253/:256 → `ZLinkActorRuntime.completeRelocationSource` :4678(:4680) → `beginTeardown`(created=false)가 3의 teardown terminal을 기다린다(commonPool, stack 기록됨).
5. 1의 barrier 연산(Join execute)은 2의 `settleSource`가 돌려준 stage를 기다리고, 그 stage는 4가 끝나야 끝난다.
→ 순환: barrier turn(1) → 정산 정리(4) → host-close teardown(3) → lifecycle 대기열에서 barrier turn(1) 뒤.
- 같은 시점에 pump 2개는 살아서 deadline wait 중이고, `permitsInUse=0`, `capacityWaiters=0`이다. 수신 경로·permit·wake는 관여하지 않는다.
- ACTOR_LEFT(command 29) 수신(`dispatchActorLeft` :5273)은 이 run에서 `handleActorLeft`의 `attempt.committed()`가 완료되지 않은 상태라(committedDone=false) 순환에 들어가지 않았다. 앞 보고의 "ACTOR_LEFT → completeRelocationSource" 추정은 정정한다: 실제 경로는 **SOURCE_LEASE_EXPIRED 정산**이다.

### ③ 결정적 재현(main 코드, sleep 없음)
- full-host 결정적 재현(실제 두 host로 "진행 중 deferred Join + 종료 teardown + SOURCE_LEASE_EXPIRED"를 gate로 강제하는 것)은 만들지 않았다. adapter·relocation 내부에 gate를 넣어야 한다.
- 대신 lifecycle lane 단위 결정적 재현을 만들었다. 브랜치의 `actors/`·`spots/`·`execution/`은 origin/main과 diff 0이다. 임시 시험(커밋하지 않음, 사본 scratchpad/TmpG1648TeardownBarrierCycleTest.java):
  - `hostCloseTeardownThenSettlementCleanupDeadlocksBehindTheJoinBarrier`: barrier turn이 정산 정리 완료를 기다리는 동안, off-turn에서 host-close `beginTeardown`(created=true) 다음 정산 `beginTeardown`(created=false)을 부른다. close teardown은 3초 상한 안에 끝나지 않고, cleanup은 실행되지 않으며, barrier도 끝나지 않는다. **재현됨.**
  - `settlementCleanupAloneOffTheBarrierTurnAlsoDeadlocks`: host close 없이 정산 정리만 off-turn에서 `beginTeardown`(created=true)을 불러도 같은 순환이다. **재현됨.**
  - 대조 `settlementCleanupOnTheBarrierTurnCompletes`: 같은 호출을 barrier turn 안에서 동기로 부르면 `isCurrent`(:398)로 기다리지 않고 끝난다. **통과.**
  - 실행: `:zlink-framework-core:test --rerun --tests '*TmpG1648TeardownBarrierCycleTest'` → 3/3 의도대로(앞의 둘은 TimeoutException 확인).
- 해석: 순환은 main 코드의 구조다. 정산 정리가 barrier turn 밖(off-turn, commonPool)에서 이어지면 반드시 멈춘다. main 0/60과 브랜치 16/30의 차이는 그 continuation이 어느 thread에서 이어지는지(turn 안 동기 vs 다른 thread)의 타이밍 차이로 보인다. 브랜치에서 왜 off-turn이 잦은지는 확정하지 않았다.

### ④ 결정 소유 표
| 결정 | 스펙 | 지금 판정 위치 | 문제 |
|---|---|---|---|
| Actor lifecycle lane의 turn 순서(barrier·teardown) | 06-state-ownership-and-lanes §5 ("재진입은 deadlock이 아니라 예외로 드러나야 한다", .en.md:200-216), 04-actor-model deferred Join barrier(:150-160) | `ZLinkActorDispatchSerials.enqueueBarrier` :430/:447, `beginTeardown` :397(:398 `isCurrent`), `startTeardown` :545/:552 | 현재 turn 판정(`isCurrent`)이 thread 기준이라 off-turn continuation을 같은 논리 turn으로 보지 못한다. 순환이 예외가 아니라 무한 대기가 된다 |
| deferred Join의 active turn 소유 | 04-actor-model deferred Join | `ZLinkActorSpotJoinCall.executeDeferred` :298, `deferredActiveTurnSeal` :311 (주석 :307-310이 "이 turn 안에서 다른 lifecycle 경계를 예약하면 자기 뒤에 서서 deadlock"이라고 이미 경고) | 같은 경고 대상인 teardown(정산 정리)은 이 handle을 쓰지 않는다 |
| expired-owner terminal 정산 정리 | 05-location-relocation/01-location-runtime §10(.en.md:1529), 04-relocation-flow §4.4 | `ZLinkActorJoinCanonicalAdapter.settleSource` :148 → `discardAfterSourceLeaseExpiry` :1417 → `cleanupLocal` :1388 → `completeRelocationSource` :4678/:4708 | barrier turn의 결과 stage가 teardown 완료를 기다린다 |
| host close의 Actor 정리 | 01-execution/03-cancellation-and-shutdown(bounded teardown) | `ZLinkActorRuntime.closeActorEntry` :4749 → `beginTeardown` | 진행 중인 barrier turn 뒤에서 기다린다(close stage에 deadline 없음, #1810) |

### 수정 선택지(적용하지 않음)
- (가) **teardown이 활성 barrier 뒤에서 기다리지 않는다**: barrier turn이 소유한 연산(정산 정리)이 부르는 teardown은 `deferredActiveTurnSeal` 같은 active-turn handle로 그 turn 안에서 실행한다. off-turn이면 그 handle로 현재 turn의 일부임을 판정한다. 판정 위치는 `beginTeardown` 하나, 기존 handle을 재사용한다(새 상태 없음).
- (나) **정산 경로가 teardown을 기다리지 않는다**: `settleSource(SOURCE_LEASE_EXPIRED)`가 barrier 결과로 teardown 완료를 기다리지 않고, teardown을 lane에 맡긴 뒤 barrier turn을 끝낸다. host-close teardown은 barrier 뒤에서 이어서 끝난다. 정산 결과의 관측(Store·authority)은 teardown terminal로 넘긴다.
- (다) host close가 Actor teardown 전에 진행 중인 deferred Join barrier를 먼저 정산·취소한다. 다만 (나)의 순환(정산 정리 단독, ③의 둘째 시험)은 남으므로 단독 해법이 아니다.
- 스펙 06 §5에 따르면 남는 순환은 최소한 예외로 드러나야 한다. (가)는 순환 자체를 없애고 규칙 수가 가장 적다(판정 1곳·기존 handle).
- **제안**: main 결함으로 별도 Issue로 분리한다(Java Actor lifecycle lane — deferred Join barrier와 expired-owner 정산 teardown의 순환). 교차언어 대조(.NET·C++·Node에 같은 구조가 있는지)는 그 Issue에서 한다. #1648은 그 수정 뒤 crossNode 30회를 다시 잰다.
- 상태: 작업 트리 clean(HEAD 57547d0b8c), 임시 계측·시험 모두 되돌림, push 없음.

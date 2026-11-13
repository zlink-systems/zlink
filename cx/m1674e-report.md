# m1674e — #1674 .NET 코드 작업

- 브랜치: `framework-dotnet/1674-per-message-cost-2`
- Worktree: `/root/worktree/zlink-1674-dotnet-2`
- 시작점: 감독자가 승인한 `6efeba1585`.
- 보드 변경·push·branch 전환·다른 프로세스 종료·공유 `~/.cache/zlink` 사용은 하지 않았습니다. 스펙 파일도 수정하지 않았습니다.

## 1. 변경과 커밋

| 커밋 | 항목 | 변경 |
|---|---|---|
| `9b26061750` | 결정 (1) | Pump 종료가 node의 제출 경계를 닫고, 즉시 send가 binding submit 전에 ShuttingDown으로 거절되게 했습니다. |
| `8d83a76c90` | 결정 (3) | Ready registry에서 owner를 독점 예약하고, 첫 receive가 FIFO prefix 확정과 drain을 한 mailbox lane turn에서 수행합니다. |
| `aaf11ed81b` | 결정 (1) 보완 | 제출 차단은 기존 node `Stopped` 상태로 처리합니다. Socket generation은 기존 receive·inbound-operation drain 뒤에 닫아 기존 control reply의 수명을 유지합니다. |
| `8f1ff31d26` | 결정 (4) | 취소할 수 없는 CancellationToken은 boxed 상수 하나를 재사용합니다. |

각 커밋 본문 마지막 줄은 `Refs #1674`입니다. 결정 (1)은 보완 커밋까지 두 커밋입니다. 초기 구현에서 socket generation을 일찍 무효화했으나, 호출 경로 점검에서 기존 control reply까지 거절할 수 있음을 확인하여 node 상태로 변경했습니다. 추가 lifecycle flag는 없습니다.

### 결정 (1): 종료 후 즉시 send

- **원인**: `SendDirectWireAsync`가 즉시 admission을 표 없이 완료하면서, pump 종료 후 물리 node dispose 전에는 table의 종료 판정을 거치지 않았습니다.
- **소유 계층**: node lifecycle. `CloseSubmissions`가 node lane과 기존 socket gate 안에서 `Stopped`를 게시하고, `SendDirectWireAsync`가 같은 socket gate 안에서 제출 전에 확인합니다. Table의 closed 상태를 send admission 정책으로 다시 사용하지 않습니다.
- **스펙**: `framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md:184`, §4 owner Close — 새 제출 차단과 이미 수락한 작업의 종료.
- **교차언어**: Java `ZLinkJavaRawMeshNode.java:4507-4512`의 close 상태와 `:7560-7565`의 requireStarted 검사를 대조했습니다(감독자 결정의 #1846). C++ `raw_mesh_node_owner.cpp:1220-1232`는 port lifetime snapshot과 native socket gate를 사용합니다. Node `raw-service-mesh-runtime.ts:1198-1221`은 close에서 router를 제거하고 `:1465-1468`에서 검사합니다. 이번 변경은 감독자가 확정한 node close 규칙을 .NET에 적용했습니다.
- **분류**: B — 승인된 M1b 뒤에 남은 lifecycle 결함.
- **Red/green**: `PumpCloseRejectsNewImmediateSendsBeforeNodeDisposal`. 수정 전에는 예외 없이 send가 성공하여 실패했고(`test-close-red.log`), 수정 후에는 ShuttingDown을 확인했습니다. 기존의 “table만 닫아도 즉시 send를 표에 등록하지 않는다” 시험도 유지했습니다(`test-close-green.log`, 2/2).

### 결정 (3): ready owner와 mailbox turn

- **소유 계층**: node의 ready registry가 owner 예약을, mailbox lane이 FIFO prefix와 record 이동·회계를 소유합니다.
- **스펙**: `04-application-job-queue-and-backpressure.ko.md:182-192`(준비된 owner 집합), `:117-118`(permit을 얻은 record의 owner handoff), `06-state-ownership-and-lanes.ko.md:165-172`(단일 FIFO turn), `02-handler-turn-and-execution-gate.ko.md:76-93`(owner 처리 순서).
- **교차언어**: C++ `service_mailbox.cpp:123-158`과 Java `ZLinkServiceMailbox.java:56-81`, Node `service-mailbox.ts:138-160`은 ready owner의 단일 claim과 FIFO prefix 이동을 소유합니다. Java scheduler의 `ZLinkServiceMailboxScheduler.java:82-120`도 ready 집합에서 owner를 빼고 drain 뒤 residue를 다시 게시합니다. 내부 자료구조는 언어마다 다르며, 이번 변경은 기존 .NET owner 순서와 record/permit 순서를 유지합니다.
- **분류**: B — 불필요한 claim turn과 전체 owner 순회 제거.
- `_readyMailboxes`의 true는 준비됨, false는 해당 owner가 예약되었음을 뜻합니다. Enqueue는 없는 항목만 추가하므로 예약을 풀지 않습니다. Release는 mailbox turn에서 residue에 따라 true로 바꾸거나 제거합니다. Idle owner는 이 registry에 없습니다.
- **시점**: `DrainReadyAsync`가 owner를 예약하고, 첫 `Receive`가 그 시점의 FIFO prefix를 확정하며 drain합니다. 첫 receive 뒤 들어온 record는 다음 claim에 남습니다. 예약만 받고 receive하지 않은 claim도 release하면 다시 준비됩니다. Record를 별도 buffer로 미리 옮기지 않았습니다.
- **대안 비교**: 첫 batch를 예약 시점에 미리 옮기는 방법은 사용하지 않았습니다. 받지 않은 batch를 release할 때 되돌리는 queue와 회계 규칙이 필요하기 때문입니다. 선택한 방법은 기존 queue와 prefix counter를 유지하고 claim/drain을 합칩니다.
- **순서**: Infrastructure 우선 → owner kind → ordinal identity의 기존 정렬을 유지합니다. Record별 application job permit을 합치거나 조기 반환하지 않습니다.
- **시험 이동**: 제거한 `TryClaimAsync`의 count 검사는 실제 첫 receive의 count 검사로 옮겼고, 독점성 검사는 node ready registry 수준에서 첫 receive 전과 receive 후/release 전에 확인합니다. FIFO payload 순서, 64+1 residue, record별 permit identity와 반환 검사는 유지했습니다. 새 순서 시험은 idle owner 32개와 준비된 actor c/a/b를 두고 a/b/c 선택 및 각 payload 1/2 순서를 확인합니다.
- `HasCurrentSource`(M3)는 수정하지 않았습니다.
- 읽는 곳이 없던 mailbox `_pendingBytes` aggregate를 삭제했습니다. Record별 `PendingBytes`와 node의 기존 queue 회계는 유지합니다.

### 결정 (4): CancellationToken boxing

- **소유 계층**: `ZLinkHandlerInvocationEngine`의 handler argument 구성 한 곳.
- **스펙**: `languages/dotnet/interfaces/05-spots.ko.md:377-395`(handler의 typed CancellationToken 인자), `01-execution/03-cancellation-and-shutdown.ko.md:17-28`(협력적 cancellation). 취소 동작이나 handler signature는 바꾸지 않았습니다.
- **교차언어**: CLR value-type boxing에 한정한 변경입니다. C++ task, Java/Kotlin stage·coroutine, Node AbortSignal의 cancellation 계약에는 변경이 없습니다.
- **분류**: B — 의미가 같은 비취소 인자의 반복 할당 제거.
- 시험은 None/default가 같은 boxed object인지, 취소 가능한 token의 identity/value와 취소 상태가 유지되는지 확인합니다(`test-token-focused.log`, 관련 42/42).

## 2. 횟수와 bytes/msg

m1674d harness를 `/root/jobs/fx/cx/m1674e/probe/`로 복사하여 사용했습니다. 각 단계는 3회 중앙값입니다. 새 시간 지표는 수집하지 않았습니다. 복사한 harness의 기존 slow-wait elapsed 기록도 제거했습니다(대기 deadline은 종료 조건으로만 사용).

- `before`: `6efeba1585`.
- `close`: 결정 (1)의 최초 적용 후. 보완 커밋은 shutdown 상태만 바꾸며 정상 처리의 lane/gate/할당 경로는 추가하지 않습니다.
- `mailbox`: 결정 (3) 후.
- `token`: 결정 (4) 후. Mailbox에서 읽지 않는 aggregate 삭제는 warmup 전에 끝나는 mailbox 초기 할당과 불필요한 정수 회계만 제거합니다.
- 각 셀 순서: **bytes/msg / lane_run / lane_post / table_gate / socket_gate**.
- 할당은 기존과 같은 `GC.GetTotalAllocatedBytes`의 process 전체 bytes/msg입니다. Gate 수는 기존 harness의 명시적 table/socket gate 계측값이며, ConcurrentDictionary 내부 stripe monitor 획득은 포함하지 않습니다. 전체 mutex 획득 수나 할당 객체 수의 예산 통과를 판정한 값은 아닙니다.

| 시나리오 | 전 | lifecycle | mailbox | token |
|---|---:|---:|---:|---:|
| remote-send | 7770.8 / 6.237 / 3.016 / 0.003 / 1.000 | 7661.7 / 6.161 / 3.016 / 0.003 / 1.000 | 5969.3 / 5.528 / 3.016 / 0.003 / 1.000 | 6048.3 / 5.601 / 3.016 / 0.003 / 1.000 |
| remote-send-window | 7718.1 / 6.169 / 3.016 / 0.049 / 1.000 | 7541.6 / 6.025 / 3.016 / 0.041 / 1.000 | 5696.9 / 5.240 / 3.016 / 0.030 / 1.000 | 5797.8 / 5.331 / 3.016 / 0.046 / 1.000 |
| remote-send-window-1k | 10806.1 / 6.124 / 3.016 / 0.053 / 1.000 | 10827.7 / 6.142 / 3.016 / 0.061 / 1.000 | 8659.0 / 5.097 / 3.016 / 0.050 / 1.000 | 8800.3 / 5.238 / 3.016 / 0.055 / 1.000 |
| remote-request | 17191.3 / 12.000 / 4.000 / 0.000 / 2.000 | 17192.1 / 12.009 / 4.000 / 0.000 / 2.001 | 16856.6 / 11.008 / 4.000 / 0.000 / 2.001 | 16851.7 / 11.000 / 4.000 / 0.000 / 2.000 |
| remote-request-1k | 20084.1 / 12.000 / 4.000 / 0.000 / 2.000 | 20088.0 / 12.000 / 4.000 / 0.000 / 2.000 | 19760.8 / 11.000 / 4.000 / 0.000 / 2.000 | 19751.8 / 11.000 / 4.000 / 0.000 / 2.000 |
| local-node-send | 2991.3 / 2.000 / 1.000 / 0.000 / 0.000 | 3003.9 / 2.000 / 1.000 / 0.000 / 0.000 | 3002.9 / 2.000 / 1.000 / 0.000 / 0.000 | 2986.4 / 2.000 / 1.000 / 0.000 / 0.000 |
| local-actor-request | 39959.4 / 33.073 / 5.000 / 4.000 / 0.000 | 40025.3 / 33.097 / 5.000 / 4.000 / 0.000 | 39266.2 / 31.097 / 5.000 / 4.000 / 0.000 | 39261.0 / 31.096 / 5.000 / 4.000 / 0.000 |

- Lifecycle 수정의 정상 경로 비용은 바뀌지 않습니다. Send의 bytes·lane 편차는 receive 회전·batching 횟수에 따라 흔들립니다.
- Mailbox 수정은 remote request 12→11 turn, local actor request 약 33.1→31.1 turn입니다. Ready가 아닌 mailbox에 claim turn을 열지 않습니다.
- Token 변경은 일반 시나리오에서 의미 있는 bytes 감소가 보이지 않았습니다. 해당 dispatch의 token이 취소 가능하면 기존 boxing이 그대로 필요합니다.

비취소 token의 효과는 같은 harness에 `handler-token` 시나리오를 추가하여 직접 확인했습니다. 인자 plan·handler·delegate는 측정 전에 준비하고 warmup 2,000회 뒤 20,000회를 호출했습니다. 원자료는 `counts-token-none-before.jsonl`, `counts-token-none-after.jsonl`이며 각 3회입니다. 앞 단계는 token 수정만 잠시 제외하고, 다음 단계는 수정한 소스를 복원한 뒤 실행했습니다.

| 직접 호출 | bytes/msg 전→후 | lane_run 전→후 | table/socket gate 전→후 |
|---|---:|---:|---:|
| CancellationToken.None | 24.1→0.1 | 0→0 | 0/0→0/0 |
| 취소 가능한 token | 24.0→24.0 | 0→0 | 0/0→0/0 |

원자료·로그·요약은 `/root/jobs/fx/cx/m1674e/`에 보존했습니다. `counts-summary.md`에는 기존 harness의 나머지 count 항목도 있습니다. 계측 코드는 제품 소스나 커밋에 남기지 않았습니다.

## 3. 최종 검증

| 검사 | 결과 | 로그 |
|---|---|---|
| .NET Release build (unit project와 참조 프로젝트) | 통과, 오류 0 | `m1674e/build-final.log` |
| Zlink.Framework.UnitTests | **3009/3009**, skipped 0 | `m1674e/test-final-unit.log` |
| Zlink.Framework.ContractTests | **96/96**, skipped 0 | `m1674e/test-final-contract.log` |
| CSharpier check | 변경 C# 8개 통과 | `m1674e/format-final.log` |
| git diff --check | 통과 | 최종 실행 |

최종 검증 실패는 0건입니다. 따라서 `/root/worktree/zlink-main-base`와 대조할 새 실패는 없었습니다. 의도한 red 시험은 수정 전 종료 창에서 send가 성공한다는 동작으로 실패했으며, 컴파일 실패를 red로 사용하지 않았습니다. 계측 4단계 × 3회와 token 직접 호출 전후 각 3회도 모두 완주했습니다.

검증 후 계측 소스·csproj 임시 항목·코드 수정용 임시 사본을 제거했습니다. 요청한 로그와 재현 harness만 jobs 디렉터리에 보존했습니다. 작업 트리는 깨끗하며 HEAD는 `8f1ff31d26`입니다.

## 4. 변경 파일과 리팩토링 점검

- `Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs`: pump 종료 시 node 제출 차단.
- `Runtime/Service/FrameworkServiceTypes.cs`: internal IMeshNode.CloseSubmissions.
- `Runtime/Service/ZLinkManagedMeshNode.cs`: lifecycle 제출 검사, ready registry 예약·재게시.
- `Runtime/Service/ZLinkMeshOwnedMailbox.cs`: claim/drain 병합, readiness callback, 사용하지 않는 aggregate 삭제.
- `Runtime/Handlers/ZLinkHandlerInvocationEngine.cs`: boxed None token 재사용.
- `tests/.../Runtime/CanonicalActorJoinIngressReplyTests.cs`: 종료 창 red/green.
- `tests/.../Runtime/MeshMailboxReadinessTests.cs`: 합친 receive 계약과 owner/FIFO/permit 순서 검증.
- `tests/.../Runtime/HandlerResultAwaiterTests.cs`: 비취소 object 재사용과 취소 상태 보존.

규칙 수(판정 위치): shutdown send 거절 1(table)→1(node), mailbox owner 독점 판정 1(TryClaim)→1(ready registry 예약), FIFO prefix 확정 1→1, token argument 선택 1→1. Mailbox claim/drain은 2 turn→1 turn입니다. 새 ready registry 하나를 추가하고 전체 mailbox scan·별도 TryClaim turn·읽지 않는 mailbox byte aggregate를 제거했습니다. 별도 queue·timer·명시적 lock은 추가하지 않았습니다.

리팩토링 점검: 성능 2건(mailbox turn/scan, token boxing)·POSDDD 1건(shutdown send 결정 소유)·불필요 코드 2건(별도 claim API, 미사용 byte aggregate)을 고쳤습니다. 작업 범위에서 넘긴 발견은 0건입니다. M3와 다음 회차 local actor published view 등은 감독자의 명시적 보류 범위로 유지합니다.

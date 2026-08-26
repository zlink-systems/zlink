# 전환 순서 조사 (codex sol, 2026-08-26)

> 감독: Claude. codex 조사 최종 보고 전문이다. 전환 순서 판정의 근거로 보존한다.

## 결론

현재 브랜치 `refactor/lane-ownership-concurrency`에서 정적으로 다시 계수한 결과, 조사한 전환 후보는 `ZLinkSerialExecutionQueue`를 제외하면 모두 **C2**다. `ZLinkSpotNodeCatalog`도 설계 문서의 “C1 비중이 클 것”이라는 예상과 달리, 48개 lock 중 24개가 여러 컬렉션을 직접 함께 사용하므로 클래스 단위 판정은 C2다.

설계표의 lock 수는 일부 파일 단위 수치였다.

- `ZLinkActorHandoffState`: 파일 63개지만 본 클래스 58개, 뒤의 `ZLinkActorMessageFollowLease`가 5개.
- `ZLinkManagedMeshNode`: 현재 파일 140개, 본 클래스 130개, 중첩·후속 클래스가 10개. 설계 문서의 141개에서 현재 브랜치가 1개 감소했다.
- `ZLinkClientServerClientRuntime.cs`: 외부 클래스 20개, 중첩 `Connection` 44개로 서로 다른 후보다.
- `ZLinkRouteMeshRuntimeService.cs`는 파일 합계가 20개지만 본 클래스 13개와 `MonitorHub` 7개로 나뉘므로 추가 후보가 아니다.

판정 기준은 [설계 §4–5](/home/hep7/project/zlink/doc/plan/concurrency-redesign/design.ko.md:46), [스펙 06 §4–6](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md:75)이다. 여러 종류가 섞이면 C2가 이긴다. 성능 메모는 [POSDDD 성능 절](/home/hep7/project/zlink/doc/principal/dev/posddd.ko.md:717)의 할당·복사·경합 기준만 적용했다.

호출 지점은 대상 트리에서 구체 타입으로 정적으로 해석 가능한 생성·메서드·속성 접근을 센 값이다. 인터페이스나 런타임 형식으로 지워진 호출은 포함하지 않았다. 비율은 호출을 포함하는 메서드가 `async`/`Task`/`ValueTask`인지에 따른 근삿값이다.

## 원래 후보

### `ZLinkSpotNodeCatalog`

[ZLinkSpotNodeCatalog.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkSpotNodeCatalog.cs:14)

- lock 48개: `_gate` 44, `_disposeGate` 4.
- `_gate` 보호 상태: `_closing`, `_instanceSpotTypes`, `_preparedSpotTypes`, `_generatedSpotCreations`, `_pending`, `_spots`, `_activeCreations`, `_creationsDrained`, `_closed`, `_idleEvictionCursor`.
- `_disposeGate`: `_disposeTask`, `_idleEvictionTask`와 종료 시작 순서.
- 여러 컬렉션 직접 접근: **24/48**. 예를 들어 L89는 6개 컬렉션, L1164는 `_instanceSpotTypes`, `_preparedSpotTypes`, `_spots`를 함께 사용한다.
- 판정: **C2**. 생성·준비·게시·종료 상태가 여러 map과 completion field를 함께 전이한다.
- 파급: **3파일/32지점**, async 약 **78%**, 동기 약 **22%**.
- 재진입 의심: **3곳**. `CloseAsync` 공개 overload→내부 overload(L1326), location callback에서 다시 `CloseAsync`를 부르는 L1150·L1468.
- 장기 작업: **3개 시작점**. idle-eviction loop(L82), reserved creation completion(L790), detached close cleanup(L1395).
- POSDDD: `_spots.Values.ToArray()` 반복(L511, L1493), relocation/list snapshot의 LINQ+정렬+배열 생성이 눈에 띈다. 여러 lifecycle map을 한 gate에 태우는 경합도 크다. 확인된 죽은 코드는 없다.

### `ZLinkActorHandoffState`

[ZLinkActorHandoffState.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorHandoffState.cs:3)

- lock **58개**, 모두 `_gate`.
- 보호 상태: `_frames`, `_sourceHoldFrames`, `_canonicalMaintenanceReplayReservations`; source/target phase, handoff/join identity, ingress admission, completion TCS, message-follow route/expiry, replay·commit counter와 flag.
- 여러 컬렉션 직접 접근: **16/58**.
- 판정: **C2**. frame 목록·phase·completion이 하나의 handoff 불변식을 구성한다.
- 파급: **14파일/121지점**, async 약 **81%**, 동기 약 **19%**.
- 재진입 의심: **3곳**. `AbortCapture()`가 `BeginAbortCaptureRestore`, `AcknowledgeAbortRestoreEnqueued`, `CompleteAbortCaptureRestore`를 다시 부른다(L1472–1477). 같은 이름의 trailing-reservation overload는 최종적으로 private core로 내려가므로 이 수에서 제외했다.
- 장기 작업: **1개**. message-follow 만료 작업(L1107, 내부 `Task.Delay` L1734).
- POSDDD: frame snapshot·commit마다 `Concat`→`OrderBy`→`ToArray`가 반복되고, L1433·L1467의 `List.RemoveAt(0)`은 frame 수에 비례한 이동 복사를 반복한다. 확인된 죽은 코드는 없다.

### `ZLinkActorRuntimeState`

[ZLinkActorRuntimeState.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorRuntimeState.cs:6)

- lock 35개: `_sessionGate` 31, `_terminalLifecycleGate` 4. 별도로 일반 actor 상태를 직렬화하는 `SemaphoreSlim _gate`가 있다.
- 보호 상태: session tombstone map, bound session, replacement, pending/source relocation route, handler activation/closed flag, terminal lifecycle completion과 dispatch mailbox 연계.
- 여러 컬렉션 직접 접근: **0/35**.
- 판정: **C2**. 컬렉션 수가 아니라 session·lifecycle field 간 불변식과 `SemaphoreSlim` 앞뒤의 비동기 행동이 결정 근거다. 스펙 §4의 semaphore 치환 금지 사례에 직접 해당한다.
- 파급: **28파일/약 530지점**, async 약 **79%**, 동기 약 **21%**.
- 재진입 의심: **약 20곳**. `ExecuteLockedAsync` 재호출 8곳, handler activation dispose 4곳, `ClearAfterDestroy` 2곳, `EnterDispatch` 2곳, generation fence 호출 4곳이 핵심이다.
- 장기 작업: **1개**. actor creation 완료 추적을 fire-and-forget으로 시작한다(L1585).
- POSDDD: tombstone pruning이 lock 안에서 key 배열을 만든다(L774–776). 현재 `SemaphoreSlim`과 두 object gate가 같은 상태의 서로 다른 부분을 직렬화해 경합 및 순서 추론 비용이 크다. 확인된 죽은 코드는 없다.

### `ZLinkManagedMeshNode`

[ZLinkManagedMeshNode.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:19)

- lock **130개**: `_gate` 97, `_socketGate` 16, `_operationGate` 8, `_disposeGate` 2, inbound gate 3, ready/entry/remote-operation gate 각 1.
- 주요 보호 상태:
  - peer/channel: `_channels`, `_peersByIntent`, `_peersByRid`, `_peerExpectations`, descriptor·routing·state·channel-selection field.
  - operation: `_operations`, `_relocationReplyOperations`, `_relocationReplyTerminals`, operation ID/source fence.
  - transport: socket, poller, monitor, receive loop, active socket generation.
  - entry/ready/inbound/remote-operation collection과 admission flag.
- 여러 컬렉션 직접 접근: **11/130**. L1807은 channel·peer map과 mailbox를, L4694는 operation 및 relocation terminal map을 함께 사용한다.
- 판정: **C2**. peer admission, socket generation, operation completion과 async send가 서로 걸쳐 있다.
- 파급: **4파일/127지점**, async 약 **20%**, 동기 약 **80%**. 구체 타입 소비는 backend adapter와 wrapper에 집중되지만 `IMeshNode` 동기 표면이 넓다.
- 재진입 의심: **5곳**: canonical join 사전 판정(L2245), `DestroyActor` overload와 operation ID(L2348–2349), reserved actor에서 `EntrySpot` 사용(L2087), 동기 `Dispose`→`DisposeAsync` (L2782).
- 장기 작업: **약 9개 시작점**. receive loop 1, request completion 2, operation expiry 4, remote user/actor operation expiry 각 1.
- POSDDD: 가장 큰 경합점은 97개 블록의 `_gate`다. wire/message 변환에서 `ToArray`, `Message.From`, metadata 복사가 매우 많고, peer snapshot도 반복 materialize한다. 일부는 소유권 계약상 필수이므로 측정 없이 제거하면 안 된다. 확인된 죽은 코드는 없다.

## 추가 후보

### `ZLinkInMemoryLocationStore`

[ZLinkInMemoryLocationStore.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Locations/ZLinkInMemoryLocationStore.cs:25)

- 두 partial 파일 합계 lock **27개**, 모두 `_gate`.
- 보호 상태: lease, mesh/client/fanout row table, entry-Spot claim·stamp, authority, scan, reservation, terminal, aggregate, active/pending capacity와 generation counter.
- 여러 컬렉션 직접 접근: **11/27**.
- 판정: **C2**. authority CAS와 aggregate commit이 여러 map·capacity counter를 함께 전이한다.
- 파급: 구체 타입 외부 사용 **0파일/0지점**. 모든 계약 메서드가 이미 `ValueTask`라 signature 파급은 최소다.
- 재진입/장기 작업: **0/0**.
- POSDDD: list API가 전체 후보를 정렬·배열화한 뒤 `Skip/Take`로 두 번째 배열을 만든다(L153–157 등). 단일 global gate가 모든 mesh와 authority를 직렬화하지만 개발·테스트용 store이므로 측정 우선순위는 낮다. 죽은 코드는 확인되지 않았다.

### `ZLinkActorHandoffAdmissions`

[ZLinkActorHandoffAdmissions.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorHandoffAdmissions.cs:7)

- lock **25개**, 모두 `_gate`.
- 보호 상태: `_pending`, `_admitting`, `_terminal`, drain epoch/signal/flag, generation cancellation source.
- 여러 컬렉션 직접 접근: **3/25**.
- 판정: **C2**. pending→admitting→terminal 전이와 drain-safe 상태가 하나의 불변식이다.
- 파급: **3파일/37지점**, async 약 **86%**, 동기 약 **14%**.
- 재진입 의심: **0곳**.
- 장기 작업: **1개**. reservation별 expiry task(L231–232, L605).
- POSDDD: request identity 캡처가 여러 byte field를 방어 복사한다(L943–951). 계약상 보존 필요성을 먼저 확인해야 한다. generation마다 expiry task가 만들어지는 비용도 측정 후보다. 죽은 코드는 없다.

### `ZLinkActorOwnershipCoordinator`

[ZLinkActorOwnershipCoordinator.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Locations/ZLinkActorOwnershipCoordinator.cs:30)

- lock **26개**: `_gate` 25, `_disposeStartGate` 1.
- 보호 상태: `_actors`, `_trackedActors`, reconciliation cancellation source, background-stopping/disposed flag, dispose task.
- 여러 컬렉션 직접 접근: **3/26**.
- 판정: **C2**. dictionary/set와 per-actor release/reconciliation task가 함께 전이한다.
- 파급: **4파일/18지점**, async 약 **78%**, 동기 약 **22%**.
- 재진입 의심: **8곳**. `ReleaseActorAfterMoveAsync`→`OwnsActor`/`ReleaseActorAsync`(L1043–1045), reconciliation→`ReleaseActorAsync`(L1141), overload forwarding과 snapshot update가 포함된다.
- 장기 작업: **1개**. activation-failure reconciliation(L1124).
- POSDDD: 동일 actor를 dictionary와 set에 중복 보관하고, drain 때 set와 task 배열을 복사한다(L1176, L1224–1227). global gate와 per-actor `WriteGate`의 이중 경합도 관찰 대상이다. 죽은 코드는 없다.

### `ZLinkClientServerClientRuntime`

[ZLinkClientServerClientRuntime.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkClientServerClientRuntime.cs:6)

- 외부 클래스 lock **20개**, 모두 `_gate`.
- 보호 상태: `_connections`, `_retired`, selection revision/plan, pending/disposed flag, manual attachment와 dispose task.
- 여러 컬렉션 직접 접근: **1/20**.
- 판정: **C2**. connection map, selection plan revision, disposal/retirement가 함께 움직이며 선택 뒤 async send/request가 이어진다.
- 파급: **5파일/14지점**, async 약 **36%**, 동기 약 **64%**.
- 재진입/장기 timer: **0/0**. dispose task는 별도 lifecycle 작업이지만 반복 timer는 없다.
- POSDDD: automatic replacement마다 desired/successor dictionary를 다시 만들고(L109–112), monitoring·dispose·selection에서 배열과 정렬 snapshot을 반복한다. 죽은 코드는 없다.

### 중첩 `ZLinkClientServerClientRuntime.Connection`

[ZLinkClientServerClientRuntime.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkClientServerClientRuntime.cs:622)

- lock **44개**: `_gate` 40, `_socketLifecycleGate` 4.
- 보호 상태: expected/current admission, ready/rejected/weight/diagnostics/identity, admission·retry task 목록, reconnect/control/liveness task, probe ID/deadline, physical/admission generation.
- 여러 컬렉션 직접 접근: **1/44**.
- 판정: **C2**. 여러 scalar field가 admission·physical-generation 불변식을 이루고 결정 뒤 reconnect/liveness async 작업이 이어진다.
- 파급: enclosing 파일 **1개/35지점**, async 약 **26%**, 동기 약 **74%**.
- 재진입 의심: **0곳**.
- 장기 작업: **5개 논리 시작점**: admission, retry, control loop, liveness loop, reconnect.
- POSDDD: readiness·diagnostics의 작은 동기 getter가 같은 gate를 공유하고, retry/admission task list를 종료 시 배열화한다(L899–900). 10ms polling/retry도 측정 후보다. 죽은 코드는 없다.

### `ZLinkFrameworkRuntime`

[ZLinkFrameworkRuntime.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkFrameworkRuntime.cs:31)

- partial 전체 lock **27개**: `_operationGate` 20, `_workerPoolGate` 3, `_remoteFrameChainGate` 2, `state.SyncRoot` 2.
- 보호 상태: operation/request counts, accepting/admission owner, epoch와 relocation fence, drain TCS, worker pools, remote-frame chain map. `state.SyncRoot`는 client bundle 및 Spot-node dictionary를 보호한다.
- 여러 컬렉션 직접 접근: **0/27**.
- 판정: **C2**. counter·flag만처럼 보이는 블록도 drain admission과 비동기 operation 수명 전체의 교차 불변식을 구성한다.
- 파급: **45파일/273지점**, async 약 **62%**, 동기 약 **38%**.
- 재진입 의심: **약 57곳**. partial 구현들이 `EnterOperation`, `ExecuteOperation[Async]`, `TryRunDetached`, `GetSpotNodeRuntime` 등의 자기 표면을 반복 호출한다.
- 장기 작업: **약 7개 시작점**. actor-join prewarm, bound-session replacement, Spot retire, remote-frame chain의 detached/delayed 작업.
- POSDDD: `EnterOperation`이 hot path마다 class형 `ZLinkRuntimeOperationLease`를 생성한다(L624, L1225). 이 할당과 central operation gate가 가장 우선적인 측정 후보다. 죽은 코드는 확인되지 않았다.

### `ZLinkSerialExecutionQueue`

[ZLinkSerialExecutionQueue.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkSerialExecutionQueue.cs:5)

- lock **25개**: `_admissionGate` 24, `_disposeGate` 1.
- 보호 상태: application/lifecycle queue, active item, pending/admitted counts, claim/sequence, relocation state, seal request/reservation.
- 여러 컬렉션 직접 접근: **2/25**.
- 판정 자체는 **C2**지만 **전환 대상에서 제외**해야 한다. 이것은 state-owner 후보가 아니라 application/lifecycle lane primitive다. 설계 §8은 lane 내부 lock을 허용하고, 스펙 §9도 `ZLinkStateLane`과 이 queue를 분리한다.
- 파급: **6파일/52지점**, async 약 **52%**, 동기 약 **48%**.
- 내부 표면 재호출 5곳, drain 장기 작업 1개가 있지만 이들은 primitive 자체의 구현 구조다.
- POSDDD: admission hot path가 모두 한 gate를 지나며 relocation seal에서 accepted-record 배열과 `Concat` 배열을 만든다. 별도 측정·primitive 최적화 작업으로 다뤄야 한다. 죽은 코드는 없다.

## 제안 전환 순서

`ZLinkSerialExecutionQueue`는 제외하고, 구체 caller 수·동기 signature 파급·내부 장기 작업을 함께 반영한 순서다.

1. `ZLinkInMemoryLocationStore` — 표본 대비 **비슷하거나 약간 낮음**. 구체 caller가 없고 표면이 이미 `ValueTask`지만 authority 불변식은 복잡하다.
2. `ZLinkActorHandoffAdmissions` — **약 0.8배**. 재진입이 없고 timer 경계가 하나다.
3. `ZLinkActorOwnershipCoordinator` — **약 1.2배**. caller는 적지만 재진입 8곳과 reconciliation 분리가 필요하다.
4. `ZLinkSpotNodeCatalog` — **약 1.5배**. 여러 컬렉션 블록 24개와 detached 작업 3개가 있다.
5. `ZLinkClientServerClientRuntime` — **약 1.0배**. lock은 적지만 동기 선택·모니터링 표면이 많다.
6. 중첩 `Connection` — **약 1.5배**. 한 파일에 갇혀 있으나 admission/reconnect/liveness의 실행 문맥 분리가 필요하다.
7. `ZLinkActorHandoffState` — **약 2배**. frame 순서·phase·completion과 121개 caller가 결합되어 있다.
8. `ZLinkActorRuntimeState` — **약 2.5배**. caller가 매우 많고 기존 semaphore 및 재진입 약 20곳을 먼저 해체해야 한다.
9. `ZLinkManagedMeshNode` — **4배 이상**. 외부 파일은 적지만 10개 gate, 130개 lock, 동기 `IMeshNode` 표면과 장기 작업 약 9개 때문에 분량이 가장 크다.
10. `ZLinkFrameworkRuntime` — **3배 이상이며 최종 배치 권장**. lock 수보다 45개 사용 파일과 partial 내부 재진입 약 57곳이 파급을 결정한다.

따라서 기존 설계 순서에서 가장 크게 바뀌는 점은 `ZLinkSpotNodeCatalog`를 C1 성격의 쉬운 후보로 보지 않는 것, 그리고 `ZLinkManagedMeshNode`보다 호출 그래프가 넓은 `ZLinkFrameworkRuntime`을 마지막에 두는 것이다.

코드·문서 수정, build/test, 금지된 Git 명령은 실행하지 않았다.


hook: Stop
hook: Stop Completed

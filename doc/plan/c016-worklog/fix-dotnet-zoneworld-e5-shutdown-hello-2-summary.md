# .NET ZoneWorld E5 admission·shutdown 구현 (D-097)

D-097(B) 승인 항목 두 개를 구현했다. (1) guard의 `Idempotent` 결과가 `Accept`와 같은
완료 진단 경로를 지난다. (2) mesh node가 host admission gate의 shutdown seal을 조회해
seal 뒤에는 새 peer admission(Hello)을 시작하지 않고, peer 전이가 Draining을 되돌리지 않는다.
새 flag·timer는 없다. Core·binding·spec·다른 언어·shared_sample·runner는 수정하지 않았다. Commit은 없다.

## 재현 환경

- Branch `main`. Package rebuild9, `.artifacts/wsl/nuget/Systems.Zlink.0.17.0.nupkg`
  SHA-256 `61085bd39d0e332fe169019d763d31e5187e94ac2ad34134f85c82036f73b664`,
  `NUGET_PACKAGES=/dev/shm/zlink-tmp-dotnet/nuget-61085bd39d0e332f`.
- `TMPDIR=/dev/shm/zlink-tmp-dotnet`, `ZLINK_LIBRARY_PATH=core/build-dev/lib`,
  `UseSharedCompilation=false`, `MSBUILDDISABLENODEREUSE=1`. dotnet은
  `flock -w7200 /tmp/zlink-dotnet-gate.lock`, sample은 추가로 `/tmp/zlink-samples-gate.lock`.

## 변경 (diff `file:line`, 변경 후 기준)

### (1) Idempotent admission의 완료 진단 통합

- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:8100`
  `admissionCompleted` 지역 변수 추가.
- `:8310-8344` — 기존 Idempotent 조기 반환(`if Hello → SendAdmission(Admit); return;`)을 제거하고
  peer 상태 갱신·`CompletePeerAdmissionUnderLock`·`publishAdmitted = true`를
  `decision == Accept` 블록으로 감쌌다. 블록 뒤 `admissionCompleted = peer.Admitted`.
- `:8346-8360` — 공통 tail: Hello면 Admit 응답(기존과 동일), `admissionCompleted`면
  `mesh_peer_admission_accepted … command={command}` 로그, `publishAdmitted`(Accept)만
  `PeerAdmitted`/`StateChanged` 발행. Idempotent는 descriptor·physical pipe·liveness epoch·
  `LastChangedMs`를 건드리지 않고 PeerAdmitted를 재발행하지 않는다.

### (2) Shutdown seal → mesh node

- `Runtime/Host/ZLinkDrainAdmissionGate.cs:115-120` `IsSealedForShutdown`
  (`_sealed != 0 && _owner == Shutdown`). Relocation fence도 `Seal()`을 호출하므로
  `IsSealed`만으로는 shutdown을 구분할 수 없다. 기존 owner 상태를 읽을 뿐 새 상태는 없다.
- `Runtime/Backend/Contracts/IZLinkBackendSpotContracts.cs:19-25`
  `SetPeerAdmissionSealGate(Func<bool>)` 기본 no-op(`SetFlowCaptureGate`와 같은 seam 패턴).
  `Runtime/Backend/DotNet/Wrappers/ZLinkBackendSpotNodeWrapper.cs:65-67` 위임.
- `Runtime/Spots/ZLinkSpotNodeInitializer.cs:27-31`
  `node.SetPeerAdmissionSealGate(() => runtime.DrainAdmission.IsSealedForShutdown)` —
  host gate가 유일한 seal 소유자, node는 조회만 한다.
- `Runtime/Service/ZLinkManagedMeshNode.cs:125-129, 246-251` `_peerAdmissionSealed` Func와 setter.
- `:8472, 8485-8491` infrastructure pump: Connecting outbound peer의 Hello 제출 직전에
  seal을 tick당 한 번 평가(`admissionSealed ??=`)하고 sealed면 `continue`. Reconnect intent와
  `NextAdmissionTimestamp`는 그대로다(물리 reconnect는 Core 소유). 이미 admit된 peer의
  Draining Update(`PublishDraining`, `:561-576`)는 그대로 전송된다.
- `:8376-8395` `SetTopologyStateUnderLock(next)`(Draining이면 무시)와
  `SetPeerLossStateUnderLock()`(`_peersByRid.Count == 0 ? Started : PartialReady`)를 하나 두고,
  admission 완료(`:8376`), liveness 만료(`:8528`), transport disconnect(`:8685`),
  control-send 실패(`:11004`) 네 곳이 모두 이것을 쓴다. 기존 `:8362-8363`의 Draining 보존
  조건을 그대로 재사용했다.

### Test

- `tests/Zlink.Framework.UnitTests/Runtime/MeshNodeShutdownSealTests.cs` (신규, csproj `:132` 등록)
  - `DrainGate_SealedForShutdown_OnlyByTheShutdownOwner` — relocation fence seal은 false, `ClaimShutdown` true, `Reset` false.
  - (a) `CrossedHelloAdmit_CompletesOnceOnBothSides` — tcp 양방향 ConnectPeer(E5 restart 형태).
    양쪽 `PeerAdmitted == 1`, `ProtocolErrors == 0`, `Ready`, admitted peer 1.
  - (a′) `IdempotentAdmit_CompletesWithoutReadmittingOrResettingTheEpoch` — DEALER로 같은 descriptor의
    Hello→Admit→Hello를 순서대로 주입(기존 `CanonicalActorJoinWireAdmissionNegativeTests.SendHelloAsync`
    패턴). 두 번째 Hello도 Admit 응답을 받고, `PeerAdmitted == 1`, `PeerRejected == 0`,
    lifecycle·revision·`LastChangedMs` 불변.
  - (b) `SealedNode_StartsNoPeerAdmissionAfterPeerLoss_AndKeepsDraining` — `ClaimShutdown` →
    `PublishDraining`(peer가 Draining Update 수신) → peer dispose → node `Draining` 유지·peer
    `Connecting` → 같은 endpoint/RID로 peer 재시작 후 2 s(Hello 간격 4회) 동안 재시작 peer의
    admitted 0·`Peers()` 비어 있음 → `DisposeAsync` 10 s 안에 완료.
  - (c) `UnsealedNode_ReadmitsRestartedPeer(false|true)` — seal 없이 (Relocate `PublishDraining` 유무)
    peer 재시작 시 양쪽 재admission. Draining 변형은 node `Draining` 유지·peer 측 `Draining`.
- 수정 전 runtime(`git stash`)에서 (b)·(c, true)·gate test가 실패함을 확인했다
  (`Expected: Draining / Actual: Started`, `Expected: Draining / Actual: Ready`). (a)·(a′)는
  진단 전용 변경이므로 수정 전후 모두 통과한다(아래 BLOCKERS).

## 네 줄

- 소유 계층: Framework mesh node가 logical Hello/Admit/Update와 그 완료 진단을 소유하고,
  Framework host `ZLinkDrainAdmissionGate`가 shutdown seal을 소유한다(node는 조회만). 물리 pipe의
  reconnect는 Core 소유이므로 outbound intent는 유지하고 Hello만 보류한다.
- Spec 조항: `05-host-relocation-flow.ko.md` §14 1단계(mesh node가 seal을 따른다, Draining Update만,
  peer 전이가 Draining을 되돌리지 않는다); `02-channel-transport/06-wire-protocol.ko.md:370` 반복
  Hello/Admit idempotence; `03-mesh-node.ko.md` §6·§7.1.
- 교차언어 대조: Java `runtime/binding/ZLinkJavaRawMeshNode.java:6842-6882 announceExpectedPeers`는
  topology에 없는 expected peer에 100 ms마다 HELLO를 재제출하며 seal 조회가 없다. Java에는 이미
  mesh별 seal 소유자 `internal/drain/ZLinkMeshDrainCoordinator.isSealed(meshName)`(`:66`)가 있으므로
  `port.send(HELLO)` 앞에서 그것을 조회하면 된다(단 Java seal은 shutdown/relocation owner를 구분하지
  않으므로 Relocate가 이 coordinator를 seal하는지 먼저 확인). Java raw node의 state는 peer 손실로
  STARTED/PARTIAL_READY로 내려가지 않아(`:766-767`) 상태 되돌림 항목은 해당 없다. C++
  `runtime/mesh/raw_mesh_node_owner.cpp:3863-3885`는 `connection_ready` edge에서 곧바로
  `send_header_only(hello)`(`~:3899-3903`)를 보내며 seal 조회가 없다 — ready edge의 hello 전송 앞에서
  host shutdown seal을 조회해야 한다. `:2897-2908`의 `duplicate_connection`은 Hello에 Admit만 응답하고
  admitted 완료 경로를 지나지 않으므로, C++ runner가 완료 trace를 읽는다면 (1)과 같은 진단 분리가
  있다. 두 언어 모두 후속 job 대상(D-097).
- 변경 분류: (1) B — 기존 결함(진단 정책 분리). (2) B — D-097이 §14를 보강해 계약이 된 뒤의 기존 결함.

## 수정 전/후 규칙 수

- (1) admission 성공 결과별 진단 정책 2개(Accept 기록/Idempotent 누락) → 1개(완료된 admission은 기록).
- (2) peer 전이 뒤 node 상태 계산 규칙: inline 3개 + 완료 경로의 Draining 보존 조건 1개(3곳에는 없음)
  → 소유자 1개(`SetTopologyStateUnderLock`)를 4곳이 사용, "Draining을 되돌리는" 예외 경로 3 → 0.
  Hello 시작 판정은 기존 seal 소유자 1개를 조회하며 새 flag·timer·상태 0.

## 검증 결과

| 검증 | 결과 | 증거 |
|---|---|---|
| 신규 unit 6개 (`--filter FullyQualifiedName~MeshNodeShutdownSealTests`) | 6 passed / 0 failed | 위 |
| 수정 전 runtime에 같은 test | 3 failed(b, c-true, gate) / 3 passed | 위 |
| ZoneWorld ×3 (`bash samples/ZoneWorld/run_sample.sh`) | 3/3 exit 0, 각 41 verdict 모두 passed(`ZW-E5-arm`·`ZW-E5` 포함), `zoneworld=completed` | `scratchpad/e5-gates/zoneworld-{1,2,3}.log`; node 로그 `/dev/shm/zlink-tmp-dotnet/tmp.{y5SaBlM4Cr,nRGx87GIBy,m1vn0fcI5H}/logs` |
| E5 restart admission 관찰 | 3/3 — zone-node-1이 재시작 node-2 RID(`zn-3680c37e`/`zn-cc6b7140`/`zn-f5b38007`)에 `mesh_peer_admission_accepted … command=Admit` 기록 | 같은 node 로그, `scratchpad/check-run.sh` |
| Seal 뒤 Hello 제출 | 3회 × 5 process 모두 `admission_sealed site=shutdown_intent` 이후 `mesh_peer_admission_sent … command=Hello` 0건, Draining `Update`는 계속 전송(node 6·gateway 3), `accepted_drained` 도달 | 같은 node 로그 |
| `bash samples/run_samples.sh` ×1 | exit 0 — TicTacToe·Bingo·SupportChat·ShoppingMall·DeliveryDispatch·GameQuest `*-placement=completed`, `zoneworld=completed` | `scratchpad/e5-gates/run_samples.log` |
| unit half (`!~CanonicalActorJoinIngressReplyTests`, `--blame-hang 10m`) | 1975 passed / 0 failed / 0 skipped, 6 m 26 s, exit 0 | `scratchpad/e5-gates/unit-half.log` |
| Idempotent 완료 로그 직접 증거 (a′를 `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1` + `dotnet test --diag`로 1회 실행, testhost stderr는 diag 로그에만 남음) | `accepted command=Hello` → 같은 descriptor의 Admit 수신 → **`mesh_peer_admission_accepted … command=Admit lifecycle=7 revision=3`** → 반복 Hello에 `sent command=Admit` + `accepted command=Hello`; PeerAdmitted는 1회 | `scratchpad/e5-gates/diag/vstest.host.*.log` |

ZoneWorld 3회에서 E5 restart admission은 모두 Accept 경로(zone-node-1의 첫 admission record가 Admit)로
완료됐고, 진단 로그에 있던 교차 Hello/Admit(양쪽 `accepted command=Hello` 뒤 Admit) 경합은 3회 중 재현되지
않았다(`check-run.sh`의 local·peer·lifecycle 키 탐지 0건). 교차 경합의 완료 로그는 위 (a′) diag 실행이 증명한다.

## BLOCKERS

- 완료 로그 텍스트는 unit test 안에서 단언하지 못한다. `ZLinkFrameworkDebugLog.SpotDiscoveryEnabled`는
  process 시작 시 env(`ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY`)를 한 번 읽는 `static readonly`라 test별로
  켤 수 없고, module initializer로 process 전체에 켜면 unit 실행 stderr가 spot-discovery 전체로 넘친다
  (tmpfs 여유 문제 포함). (a)/(a′)는 tail 공유의 불변식(Admit 응답 유지, PeerAdmitted 1회, epoch·revision
  불변)을 단언하고, 로그 자체는 위 표의 `--diag` 1회 실행으로 증명했다(수동 증거, suite에는 없음).
  로그를 unit에서 상시 단언하려면 진단 sink seam이 필요하며 이는 별도 판정이다.
- Sealed node에 도착한 inbound Hello는 여전히 Accept 경로로 admit되고 Admit(runtime-state=Draining)을
  응답한다. §14 1단계의 "새 peer admission을 시작하지 않는다"를 '응답도 거부'로 읽으면 추가 변경이
  필요하다. 현재 구현은 시작(Hello 제출)만 막고, 응답은 Draining descriptor 게시와 같은 효과이므로 두었다.
- Relocate rollback 뒤 mesh node의 Draining 해제 경로가 없다(`_state`를 Draining에서 되돌리는 코드는
  없음; `RestoreServing`/`ReopenRelocationAdmissions`는 mesh node를 건드리지 않음). 수정 전에는 peer
  disconnect→readmit이 우연히 Ready로 되돌렸으나 이제 완료 경로와 같이 Draining을 보존한다. 기존 gap이며
  이 job 범위 밖이다.

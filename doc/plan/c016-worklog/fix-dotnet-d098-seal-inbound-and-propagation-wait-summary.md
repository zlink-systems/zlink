# .NET D-098: inbound Hello seal과 publication terminal

D-098 item 3·4의 승인 범위를 구현했다. Shutdown seal 뒤 inbound Hello는 peer 생성·변경이나
`PeerAdmitted` 발행 없이 종료하며 Admit을 보내지 않는다. 이미 admit된 peer의 Update와 기존
peer에게 보내는 Draining Update는 유지한다. Drain은 Draining 게시 terminal을 기다린 뒤 다음
단계로 진행하며, 게시 성공 뒤의 시간 대기는 제거했다. Commit은 하지 않았다.

## 변경 파일과 위치

아래 위치는 변경 후 기준이며 경로는 `framework/languages/dotnet/` 기준이다.

| 파일:행 | 변경 |
|---|---|
| `src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:8088` | `ProcessAdmissionCore`의 peer 조회·생성 전에 Hello와 기존 `_peerAdmissionSealed` 조회 결과를 확인한다. 새 flag 없이 host의 `IsSealedForShutdown` 배선을 사용한다. 선택적 gate가 없는 standalone node도 기존 admission을 유지한다. |
| `src/Zlink.Framework/Runtime/Host/ZLinkFrameworkDrainExecutor.cs:122` | marker·serving-weight publication terminal 다음에 accepted work를 기다린다. `WaitForDescriptorPropagationAsync`, `CalculatePropagationDelay`, jitter 상수와 관련 trace를 삭제했다. 기존 실패 처리와 publication 재시도는 변경하지 않았다. |
| `tests/Zlink.Framework.UnitTests/Runtime/MeshNodeShutdownSealTests.cs:148` | sealed/unsealed inbound Hello를 각각 검증한다. Sealed이면 Admit·peer·PeerAdmitted·PeerRejected가 없고 node 상태가 유지된다. Unsealed이면 Admit 응답과 admission 1회를 확인한다. |
| `tests/Zlink.Framework.UnitTests/Runtime/MeshNodeShutdownSealTests.cs:207` | 기존 peer를 admit한 뒤 seal하고 revision 99 Hello와 revision 4 Draining Update를 보낸다. Hello가 descriptor를 변경하지 않고 Update가 revision 4·Draining으로 반영되며 Admit이 없는지 검증한다. 기존 outbound Draining Update 테스트도 유지한다. |
| `tests/Zlink.Framework.UnitTests/Runtime/DrainCoordinatorTests.cs:700` | marker 또는 serving-weight publication을 미완료 Task로 보류한다. Terminal 전에는 accepted work·callback·cleanup이 시작하지 않고, 성공 terminal 후에는 polling 2초와 무관하게 기존 1초 검증 기한 안에 완료한다. |
| `tests/Zlink.Framework.UnitTests/Runtime/DrainCoordinatorTests.cs:18` | 기존 publish → accepted work → callbacks → cleanup 전체 순서 단언을 유지한다. 시간 계산 단언·전용 probe 신호를 제거하고 음수 polling fixture를 정상 값으로 바꿨다. |

## 네 줄

- 소유 계층: Framework host `ZLinkDrainAdmissionGate`가 shutdown seal을 소유하고 mesh node는 조회한다. Logical Hello/Admit/Update는 Framework mesh, shutdown publication과 후속 단계 순서는 Framework drain executor가 소유한다.
- Spec 조항: `05-location-relocation/05-host-relocation-flow.{ko,en}.md` §14 step 1의 “새 peer admission을 시작하지도 수락하지도 않는다”와 step 2~5의 publication → accepted work → closing callback → cleanup 순서.
- 교차언어 대조: 조사 시 Java `ZLinkJavaRawMeshNode.java:6520`의 inbound Hello도 무조건 Admit을 응답해 같은 결함이었다(다른 job 범위). Java `runtime/host/ZLinkFrameworkRuntime.java:2095`의 `runDrain`은 `markDraining` terminal 뒤 barrier로 진행하며 별도 시간 대기가 없다. Node `runtime/host/index.ts:1884`의 `publishHostDraining`도 성공 즉시 반환한다(`8159b15752`). 시간 대기는 .NET에 남아 있던 기존 결함이다.
- 변경 분류: item 3 B — D-098·개정 §14에 대한 기존 admission 결함. Item 4 B — 게시 terminal과 무관한 시간 대기를 추가하던 기존 결함.

## 수정 전/후 규칙 수

- Mesh admission의 seal 정책: outbound는 seal 적용 / inbound는 무조건 수락의 2개 → 양방향 모두 기존 host seal을 따르는 1개. Seal 소유자 1 → 1, 새 flag·timer·상태 0.
- Publication 완료 판정: terminal + propagation 시간의 2개 → terminal 1개. 시간 계산 helper와 jitter 상수 제거.
- 대안 비교: 각 Admit 송신 분기에 seal 검사를 복제하면 peer 생성·변경을 막지 못한다. Admission 진입점의 한 검사로 상태 변경과 모든 Hello 응답 분기를 함께 차단한다. Propagation 시간을 줄이는 대안은 별도 시간 규칙이 남으므로 채택하지 않았다.

## 환경과 검증

- Branch `main`. 이 job의 변경은 위 .NET 4개 파일과 이 요약 파일뿐이다.
- Rebuild10: `.artifacts/wsl/nuget/Systems.Zlink.0.17.0.nupkg` SHA-256
  `5827572985a089510f5ed67082be521262bc66fbc56e74672b7700ac0367412e`.
  Package provenance와 `core/build-dev/lib/libzlink.so` SHA-256은 모두
  `785b647b3fa1026959009e6cde6b18b470e438d807b6befdb2e96f0462c027a6`.
- `TMPDIR=/dev/shm/zlink-tmp-dotnet`,
  `NUGET_PACKAGES=/dev/shm/zlink-tmp-dotnet/nuget-5827572985a08951`,
  `ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib`,
  `UseSharedCompilation=false`, `MSBUILDDISABLENODEREUSE=1`, telemetry off.
- 모든 dotnet 실행: `flock -w7200 /tmp/zlink-dotnet-gate.lock`.
  Sample은 추가로 `flock -w7200 /tmp/zlink-samples-gate.lock`.
  로그·검사 script·환경은 `/tmp/zlink-d098-dotnet/`에 보존한다.

| 검증 | 결과 | 증거 (`/tmp/zlink-d098-dotnet/` 기준) |
|---|---|---|
| DrainCoordinatorTests + MeshNodeShutdownSealTests + admission·weight 관련 owner 테스트 | 최종 unit half의 TRX에서 각각 48·9·10·15·6 passed, 합계 88 passed / 0 failed | `unit-results/unit-half.trx` |
| ZoneWorld ×2 | 2/2 exit 0, 각 고유 verdict 41개 passed, E5-arm·E5와 `zoneworld=completed` 확인 | `zoneworld-1.log`, `zoneworld-2.log` |
| E5 restart admission | 2/2: node-1이 재시작 peer `zn-56d06d52-4684-4905-a019-3c262a9b9d07`(Hello), `zn-cd2109d2-98d3-4cf5-b8c9-88b144aa0cd8`(Admit)의 admission 완료 기록 | `zoneworld-1/ZoneWorld/logs/zone-node-1.log:16728`, `zoneworld-2/ZoneWorld/logs/zone-node-1.log:16006` |
| Seal 이후 전송 로그 | 최종 replacement node를 포함해 2회 합계 12 process: Hello 0, Admit 0, Draining Update 66, `accepted_drained` 12. 이 실행들에서 seal 이후 inbound Hello 자체는 0건이며, 그 입력에 대한 무응답·무상태변경은 unit이 검증한다. | `zoneworld-{1,2}-check.json`, `check_logs.py` |
| Unit half ×1 (`--filter 'FullyQualifiedName!~CanonicalActorJoinIngressReplyTests'`) | exit 0, 1978 passed / 0 failed / 0 skipped, 2분 32초 | `unit-half.log`, `unit-results/unit-half.trx` |
| `bash samples/run_samples.sh` ×1 | exit 0, TicTacToe·Bingo·SupportChat·ShoppingMall·DeliveryDispatch·GameQuest의 `*-placement=completed`, ZoneWorld의 `zoneworld=completed` 모두 확인 | `samples.log`, `samples/<sample>/logs/` |

## BLOCKERS

남은 실패와 구현 blocker는 없다. 보호 경로·다른 언어·shared_sample은 이 job에서 수정하지 않았다.

확장 focused 실행에서는 기존 `Canonical_actor_join_malformed_body_returns_protocol_error`가
admission 응답 2초 timeout으로 1회 실패했다(`owners.log`: 87 passed / 1 failed).
해당 fixture의 `ReceiveAsync`는 `DateTime.UtcNow`를 사용하지만, 당시 원인은 확정하지 않았다.
동일 코드의 단독 trace 실행(`isolated.log`, `diag/isolated.log`)과 위 최종 unit half는 통과했다.
해당 fixture·timeout·assertion은 수정하지 않았다.

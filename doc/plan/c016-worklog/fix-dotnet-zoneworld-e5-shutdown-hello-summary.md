# .NET ZoneWorld E5 admission·shutdown 진단

구현 전 감독 검토 기록이다. 보존 로그의 E5 실패는 shutdown seal 이전에 발생했다.
양쪽 node가 Hello를 수락한 뒤 받은 동일 descriptor의 Admit가 idempotent 경로에서
완료 로그 없이 반환되어 runner의 admission 증거 조건을 만족하지 못한다.
Post-seal Hello는 별도 현상이며, 기존 actor handoff drain 대기의 원인으로 확인되지 않았다.
Runtime·test·sample 소스는 수정하지 않았다. Commit은 없다.

## 재현 환경

- Branch: `main`. 기존 다른 언어·binding 변경은 유지한다.
- Package: rebuild9, `.artifacts/wsl/nuget/Systems.Zlink.0.17.0.nupkg`.
- Package SHA-256: `61085bd39d0e332fe169019d763d31e5187e94ac2ad34134f85c82036f73b664`.
- Package Linux x64 native와 `core/build-dev/lib/libzlink.so` SHA-256:
  `2055a5819059c91be6afc8c50073f22001bb59598ecf7424045918306ef9f9a0`로 일치한다.
- 환경: `/tmp/zlink-e5/env.sh`. `TMPDIR=/dev/shm/zlink-tmp-dotnet`,
  `ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib`,
  `NUGET_PACKAGES=/dev/shm/zlink-tmp-dotnet/nuget-61085bd39d0e332f`,
  `UseSharedCompilation=false`, `MSBUILDDISABLENODEREUSE=1`,
  `DOTNET_CLI_TELEMETRY_OPTOUT=1`.
- .NET 실행은 `flock -w7200 /tmp/zlink-dotnet-gate.lock`을 사용한다.
  Sample은 바깥에서 `flock -w7200 /tmp/zlink-samples-gate.lock`도 획득한다.
  기존 gate가 사용 중인 lock을 기다린다. Runner wait는 변경하지 않는다.
- 재현 명령: `/tmp/zlink-e5/baseline.sh`, 전체 ZoneWorld 3회.
  각 실행은 `/tmp/zlink-e5/before-N/runner.log`와 `evidence/ZoneWorld/logs/`에 보존한다.

## 보존 로그의 실제 순서

기준 디렉터리는
`/tmp/claude-1000/-home-hep7-project-zlink/be443aad-d720-4c23-9860-099f913fdfd4/scratchpad/`다.
아래 node 로그는 그 아래 `zoneworld-e5-fail-logs/`에 있다.

1. `gate9a-dotnet-zoneworld-2.log:123-126`: E5-arm 통과 후 PID 91236을 종료하고
   PID 91643을 시작한다. E5는 `framework/languages/dotnet/samples/ZoneWorld/run_sample.sh:911-915`에서
   `stop_node`를 호출한다. `:225-237`의 구현은 SIGKILL이며 graceful drain이 아니다.
   Graceful stop은 앞선 C2에서 사용한다.
2. 새 node RID는 `zn-ba5a0e2c-7fb1-42b5-b140-92bf571cd966`, lifecycle은
   `5074490627208057463`이다. 약 19:26:29.98에 양쪽 Hello가 교차한다.
   `zone-node-2.log:17205,17210,17212`는 Hello 수신·수락 후 Admit 수신이다.
   `zone-node-1.log:17266-17270`도 Hello 수신·수락 후 Admit 수신이다.
   양쪽 모두 `mesh_peer_admission_accepted ... command=Hello`를 기록한다.
3. `zone-node-1.log:17319`의 endpoint handover는 약 19:26:30.38이다.
   이전 RID `zn-ecacc...`의 연결 의도를 제거하고 이미 admission한 `zn-ba5a...`를
   기대값으로 설정한다. 위 Hello/Admit 교환 이후이며 seal 이전이다.
4. Runner는 `run_sample.sh:403-426`에서 `accepted ... command=Admit`만 기다린다.
   `gate9a-dotnet-zoneworld-2.log:127`에서 그 대기가 실패한다.
5. `zone-node-2.log:21202-21206`: **19:27:52.430**에 application shutdown이 시작되고
   **19:27:52.447 무렵** seal한다. 같은 때 node-1도 seal한다(`zone-node-1.log:25401-25405`).
   실패한 runner의 EXIT cleanup이 process들을 정리하는 시점이다.
6. `zone-node-2.log:21451-21452`: accepted operations를 지난 뒤 actor handoff를 기다린다.
   `:21461,21477`에 같은 lifecycle의 Hello가 두 번 기록된다.
   Node-1은 **19:27:58.687**에 Stopped다(`zone-node-1.log:25652`).
   `:21493`의 peer 제거는 Store live snapshot이 자기 node 하나만 남은 다음이다.
   해당 debug 행에는 timestamp가 없어 두 Hello와 peer 제거의 정확한 시각은 확정하지 않는다.
7. `zone-node-2.log:21679,21686`에서 기존 `bot-se-x`, `bot-ne-x` handoff admission이
   각각 **19:28:04.873**, **19:28:05.098**에 만료된다. 즉시 `accepted_drained`로 진행하고
   **19:28:05.213**, `:21695`에서 `Stopped/None`이다. Seal부터 약 12.8초이며 70초가 아니다.

Host clock 변동을 원인으로 사용하지 않는다.

## 원인과 수정 제안

### E5 완료 로그 누락

`framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkServiceAdmissionGuard.cs:48-51`은
같은 lifecycle·revision·descriptor의 후속 Admit를 `Idempotent`로 판정한다.
`ZLinkManagedMeshNode.cs:8298-8305`는 그 성공 경로에서 조기 반환하여
`:8340`의 완료 로그를 건너뛴다. 동일한 Hello/Admit 교환이라도 먼저 받은 command에 따라
runner가 관찰할 완료 로그가 달라진다. 실제 수신 실패로 분류할 근거는 없다.

제안은 guard가 허용한 idempotent admission도 기존 완료 진단 경로를 공유하도록 하는 것이다.
Descriptor·liveness epoch를 재설정하거나 PeerAdmitted 이벤트를 중복 발행해서는 안 된다.
Runner assertion 변경 대안은 선택하지 않는다. 대기 시간과 `command=Admit` 조건을 유지한다.

### Shutdown 동안의 Hello

Host의 기존 seal 소유자는 `ZLinkDrainAdmissionGate.cs:99-115`다.
`ZLinkFrameworkRuntime.cs:444-455`가 이를 seal하지만 mesh에는 그 gate가 전달되지 않는다.
`ZLinkManagedMeshNode.cs:8437-8468`의 infrastructure pump는 outbound Connecting peer에
500ms 간격으로 Hello를 제출하고, `:10621-10665`의 송신 경로에는 host seal 조회가 없다.

Connecting으로 전환하는 후보 경로는 transport disconnect(`:8607-8663`), liveness expiry
(`:8476-8499`), control-send failure(`:10930-10978`)다. 보존 로그만으로 두 Hello 직전의
정확한 전환 경로를 확정할 수 없다. Expectation re-arm이 직접 Hello를 보냈다는 증거도 없다.
이 전환들은 node의 `_state`를 Started/PartialReady로 쓰므로 기존 Draining publication을
덮어쓸 수 있다. `:8362-8363`의 admission 완료 경로에는 Draining 보존 조건이 있지만 이 세 경로에는 없다.

제안은 새 shutdown flag·timer 없이 기존 host seal을 admission 시작 판정에 연결하는 것이다.
`_state == Draining`만 검사하는 대안은 Relocate도 같은 상태를 사용하고 위 peer 전환에서
상태를 덮어쓰므로 seal을 대신하지 못한다. 기존 peer의 Draining Update와 accepted work는
유지해야 한다. Host seal과 peer handshake의 공통 계약 경계는 감독 판정이 필요하다.

### Drain 대기의 소유자

`ZLinkFrameworkRuntime.cs:611-616`은 host actor admission과 actor handoff admission을 기다린다.
`ZLinkActorHandoffAdmissions.cs:29-32`는 기존 `_drainSafe` task를 기다린다.
Mesh Hello의 control-send task를 이 대기 조건에 포함하지 않는다.
보존 로그는 pending actor handoff 만료로 drain이 끝났음을 보여준다. Hello 금지만으로
이 actor handoff 대기가 줄어든다고 주장하지 않는다.

## 소유권·계약 판정

- 소유 계층: Framework mesh가 logical Hello/Admit/Update와 그 진단을 소유하고,
  Framework host admission gate와 actor handoff owner가 shutdown seal·accepted-work drain을 소유한다.
  Physical reconnect·handover·completion은 Core/binding 소유다.
- Spec 조항: `05-host-relocation-flow.ko.md:759-828` 및 `.en.md:843` §14,
  `03-mesh-node.ko.md` §6·§7.1, `02-channel-transport/06-wire-protocol.ko.md:370`의
  반복 Hello/Admit idempotence. §14는 application admission과 새 relocation unit을 명시하며,
  모든 mesh handshake 금지를 직접 명시하지는 않는다.
- 교차언어 대조: Java `runtime/binding/ZLinkJavaRawMeshNode.java:6842-6882`도 topology에
  admitted peer가 없으면 Hello를 다시 제출하며 host seal 조회가 없다. C++
  `runtime/mesh/raw_mesh_node_owner.cpp:3863-3885`는 READY edge로 admission을 시작한다.
  `:2897-2908`은 duplicate admission에 Hello의 Admit만 응답하고 반환한다.
  .NET 전용 증거 누락은 idempotent 분기의 debug log와 .NET runner 조건의 조합이다.
  Shutdown mesh 차단은 다른 언어에도 검토가 필요한 공통 동작 차이다.
- 변경 분류: E5 진단 누락은 **B — 기존 결함, 구현 승인 대기**.
  Shutdown mesh 차단은 사용자 요구이나 §14의 application seal과 구분되는 범위이므로
  공통 계약 판정을 요청한다. Runtime 변경은 아직 없다.
- 수정 전/후 규칙 수: **현재 변경 없음**. E5 수정안은 guard 성공 결과별 진단 정책
  2개(Accept는 기록, Idempotent는 누락)를 성공 진단 1개로 통합한다.

## 검증 결과

| 검증 | 결과 | 증거 |
|---|---|---|
| 기존 admission guard·Draining Update·shutdown 순서 focused | 3 passed / 0 failed / 0 skipped | `/tmp/zlink-e5/diagnosis-focused.log`, `tests/diagnosis-focused.trx` |
| 수정 전 전체 ZoneWorld ×3 | 기존 sample gate lock 대기 | `/tmp/zlink-e5/baseline.sh` |
| 신규 회귀·수정 후 ZoneWorld ×3·aggregate·unit half | 구현 승인 후 실행 대상 | 미실행 |

독립 리뷰에서 보존 로그 인과와 소스 근거를 대조했으며, timestamp 정밀도와 인용 위치 지적을 반영했다.

## BLOCKERS

- 루트 `AGENTS.md` §3의 “감독이 A 또는 B로 승인한 뒤에만 2단계 구현” 규칙에 따라
  위 진단과 구체적인 수정 범위 승인을 요청했다. 아직 승인을 받지 않았다.
- 원래 요청의 “seal 때문에 restart가 70초 지연됨”이라는 인과는 보존 로그와 다르다.
  E5 admission 로그 누락과 post-seal Hello를 분리해 판정해야 한다.
- 정확한 Connecting trigger와 shutdown mesh-handshake 금지의 공통 계약 경계가 남아 있다.

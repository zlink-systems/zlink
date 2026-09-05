# Java mesh shutdown seal·admission 진단 정합성 (D-097)

Java raw mesh node가 host의 기존 shutdown admission seal을 조회하도록 연결했다.
Seal 뒤에는 Hello를 재제출하지 않으며, shutdown이 raw node와 wire descriptor를
Draining으로 게시한다. 신규·동일 admission은 기존의 공통 성공 경로를 유지하고
그 끝에서 같은 완료 진단을 기록한다. Branch는 `main`이며 commit하지 않았다.

## 원인과 변경 근거

- 수정 전 `ZLinkJavaRawMeshNode.java:6874-6911`의 `announceExpectedPeers`는
  topology에 admitted peer가 없으면 100 ms마다 Hello를 제출하며 host seal을 읽지 않았다.
- `ZLinkFrameworkRuntime.java:2064`의 `meshDrains.sealAll()`은 `drain()`에서만
  호출된다. Relocate는 이 coordinator를 seal하지 않는다. 따라서 Java에서는
  `ZLinkMeshDrainCoordinator.isSealed(meshName)`이 shutdown 전용 조회로 충분하다.
- 수정 전 raw node의 descriptor 게시에는 Serving·weight 변경만 있었으며
  Draining 게시가 없었다(`ZLinkJavaRawMeshNode.java:808-841`). Host/store의 Draining과
  wire descriptor가 분리돼 있었다. Peer 손실은 raw node의 lifecycle state를
  변경하지 않으므로, .NET처럼 peer 전이마다 Draining 보존 조건을 추가할 필요는 없다.
- Java에는 .NET의 `Idempotent` 조기 반환 결함이 없다.
  `ZLinkServiceTopologyRegistry.java:109-111,155-173`은 같은 connection의 반복 descriptor를
  `ADMITTED`로 반환한다. `ZLinkServiceLivenessRegistry.java:90-92`도 같은 connection의
  `admit`에서 기존 상태를 보존한다. 이번 변경은 이 공통 성공 경로에 완료 진단을 두며,
  별도 Idempotent 분기나 새 admission 결과를 만들지 않는다.

두 안을 비교했다. Raw node에 shutdown boolean을 복제하면 seal 소유자가 둘이 된다.
기존 coordinator를 읽는 predicate를 연결하면 소유자는 하나이고 Relocate도 구분되므로
후자를 택했다. Draining 전용 descriptor 전송 루프를 추가하는 대신 기존 게시 루프
세 개를 하나로 모았다.

## 변경 파일 (변경 후 file:line)

경로의 공통 앞부분은 `framework/languages/java/zlink-framework-core/`다.

| 파일 | 변경 |
|---|---|
| `src/main/java/systems/zlink/framework/runtime/internal/backend/ZLinkInternalMeshNode.java:32` | Internal seam `setPeerAdmissionSealGate`와 `markServiceDraining`. Application public API는 추가하지 않았다. |
| `src/main/java/systems/zlink/framework/runtime/host/ZLinkFrameworkRuntime.java:315` | 각 raw node가 자기 mesh의 기존 `meshDrains.isSealed`를 조회하도록 배선한다. |
| 같은 파일 `:2095` | Seal 이후 `runDrain`에서 admitted peer에 Draining descriptor를 게시하고 기존 accepted-work barrier를 기다린다. |
| `src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:237,813,6906` | Seal predicate를 보관하고 Hello 재제출 직전에 조회한다. Seal이면 announcement 시각도 갱신하지 않는다. 기존 intent·transport 식별·교체 규칙은 유지한다. |
| 같은 파일 `:818,834` | `markServiceDraining`이 node state와 descriptor를 전이한다. `publishLocalDescriptor`가 weight·Serving·Draining의 wire Update 게시를 함께 소유한다. 반복 Draining은 revision을 올리지 않는다. |
| 같은 파일 `:6533` | 기존 `ADMITTED` 공통 tail에서 `mesh_peer_admission_accepted peer=… command=Hello/Admit/Update lifecycle=… revision=…`를 기록한다. Hello 제출 진단은 `:6910`. 둘 다 기존 `STREAM_TRACE` gate 뒤에서만 문자열을 생성한다. |
| `src/test/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNodeShutdownSealTest.java:31` | 실제 inproc 연결: seal → Draining Update 수신 → peer 종료 → replacement intent → Hello 없음·Draining 유지 → accepted claim 해제와 drain 완료. |
| 같은 파일 `:89` | Seal 없는 일반 상태와 Draining 상태에서 replacement peer의 재admission을 각각 검증한다. |
| 같은 파일 `:127` | 양방향 연결과 반복 Hello/Admit에서 양쪽 완료 진단, 같은 liveness 상태 객체, peer 1개와 PEER_READY 중복 없음 검증. 로그 검증은 `ZLINK_JAVA_STREAM_TRACE=1`일 때 실행한다. |
| `src/test/java/systems/zlink/framework/runtime/host/ZLinkFrameworkRuntimeDrainRouteTest.java:125` | 실제 host shutdown에서 같은 seal 조회가 false→true로 바뀌고 raw node가 Draining이 되며, accepted claim이 끝난 후 `Stopped/None`으로 종료하는지 검증한다. |

## 네 줄

- **소유 계층:** Framework host `ZLinkMeshDrainCoordinator`가 shutdown seal·accepted-work barrier를 소유하고 raw mesh node는 seal 조회·logical Hello/Admit/Update·완료 진단을 소유한다. Physical reconnect는 Core 소유다.
- **Spec 조항:** `05-location-relocation/05-host-relocation-flow.ko.md` §14 1단계(D-097 보강), `03-spot-actor/03-mesh-node.ko.md` §6·§7.1. Wire의 반복 Hello/Admit idempotence는 `02-channel-transport/06-wire-protocol.ko.md`의 admission 규칙을 따른다.
- **교차언어 대조:** `.NET` D-097 구현은 `IsSealedForShutdown`을 조회하고 Accept/Idempotent 공통 tail에서 완료를 기록한다. Java coordinator는 shutdown에서만 seal되므로 owner 분류가 필요 없고, registry도 이미 두 성공을 `ADMITTED`로 합친다. Java의 차이는 구조 차이이며 Core·binding 보상이 아니다.
- **변경 분류:** B — D-097 계약의 shutdown seal 조회·Draining wire 게시 누락을 수정하고, 기존 공통 admission 성공에 완료 진단을 연결했다. Java에 없는 Idempotent 분기 결함을 만들어 수정하지 않았다.

## 수정 전/후 규칙 수

- Admission을 닫는 판정: application은 host seal, Hello는 별도 주기 판정 2 → host seal 하나를 공유 1. 새 shutdown boolean·timer·generation 상태 0.
- Local descriptor의 wire Update 게시 소유자: weight 두 곳·Serving 한 곳 3 → `publishLocalDescriptor` 1. Draining도 같은 소유자를 사용한다.
- Admission 성공 처리·진단 경로: Java는 이미 1 → 1. 완료 진단을 공통 tail에 추가했으며 결과별 분기·PeerReady 발행 규칙을 늘리지 않았다.
- Peer 손실의 node state 변경 규칙: 0 → 0. Draining을 되돌리는 전이를 추가하지 않았다.

## 환경과 검증 결과

- 작업 위치: `framework/languages/java`.
- `TMPDIR=/dev/shm/zlink-tmp-java`, `unset ZLINK_LIBRARY_PATH`, `ZLINK_JAVA_STREAM_TRACE=1`.
- Gradle: `flock -w7200 /tmp/zlink-jvm-gate.lock`. Sample: 추가로
  `flock -w7200 /tmp/zlink-samples-gate.lock`, `ZLINK_SAMPLE_KEEP_RUN_DIR=1`.
- Maven `.artifacts/wsl/maven/systems/zlink/zlink/0.17.0/zlink-0.17.0.jar` 안의
  `native/linux-x86_64/libzlink.so` SHA-256:
  `2055a5819059c91be6afc8c50073f22001bb59598ecf7424045918306ef9f9a0`.
- 증거: `/tmp/zlink-java-mesh-shutdown-seal/`. Core·binding package를 다시 만들지 않았다.

| 검증 | 결과 | 증거 |
|---|---|---|
| 최초 focused: 새 raw node 회귀 + drain coordinator | 6/6 PASS, skipped 0 | `focused.log`, `focused-xml/` |
| Host drain class | 7/7 PASS | `host-focused.log`, `host-focused-xml/` |
| `:zlink-framework-core:test contractTest --continue` 1회 | Core: Gradle 1,252개 중 1 FAIL; XML leaf 1,239개 중 1 FAIL. Contract 96/96 PASS | `gate.log`, `gate.exit`, `gate-xml/zlink-framework-core/`, 각 module의 `contractTest/` |
| Java `ZLINK_SAMPLE_LANGUAGES=java bash samples/run_samples.sh` 1회 | 7/7 PASS, E5·E5-arm PASS, exit 0 | `java-samples.log`, `java-samples.exit` |
| Kotlin `bash samples/kotlin/ZoneWorld/run_sample.sh` 1회 | 전체 시나리오·E5·E5-arm PASS, `zoneworld=completed`, exit 0 | `kotlin-zoneworld.log`, `kotlin-zoneworld.exit` |
| 관련 class 4개 재검증 | M6A 28/28, transport identity 6/6, host drain 7/7 PASS. 새 class 3 PASS / 진단 1 FAIL | `final-focused.log`, `final-focused-xml/` |
| 최종 테스트 준비 조건 수정 후 새 class | 4/4 PASS, skipped 0, exit 0 | `diagnostic-boundary.log`, `diagnostic-boundary-xml/` |
| 변경 파일 whitespace | `git diff --check` PASS | 최종 작업 tree |

Core gate 집계에는 기존 Kotlin `test` 결과를 포함하지 않았다. 실제 실행한 Kotlin
검증은 `contractTest` 17개와 ZoneWorld다. 전체 gate에서 runtime 코드는 최종 버전이며,
그 뒤 바뀐 Java 파일은 새 진단 테스트의 준비 조건뿐이다.

### E5 admission 증거

Java ZoneWorld 로그:
`/dev/shm/zlink-tmp-java/tmp.G2Vywde9uD/logs/`.
재시작 node-2 RID는 `zn-076ab6b9-e9c6-4524-8d0d-7c931fd46dc5`다.

- `zone-node-1.log:88934`: 재시작 node-2의 `mesh_peer_admission_accepted … command=Hello`.
- `zone-node-2.log:101834`: 재시작 node-2가 node-1
  (`zn-81582f24-f9b6-4365-bc72-1f6a2e9c465f`)을 받은
  `mesh_peer_admission_accepted … command=Admit`.
- 위 로그에서 E5는 작은 RID인 재시작 node-2가 Hello를 시작한 경우다.
  Node-1에서 `command=Admit`가 나왔다고 주장하지 않는다. 양쪽 완료 진단을
  `/tmp/zlink-java-mesh-shutdown-seal/java-e5-admission.txt`에 별도로 보존했다.
- Kotlin ZoneWorld 로그: `/dev/shm/zlink-tmp-java/tmp.3pNtX0hjtk/logs/`.

### 진단 테스트의 실패와 최종 경계

전체 gate의 유일한 실패는 새
`crossedHelloAdmitUsesCompletionDiagnosticOnBothSidesWithoutResettingLiveness`였다.
최초 wire admission의 임시 logical connection ID를 기록한 뒤 늦은
`CONNECTION_READY`가 같은 연결의 monitor identity를 등록했고, 재검증 ACK가
`PEER_READY`를 다시 기록해 “중복 없음” 단언이 실패했다. 양쪽 Hello/Admit 완료
진단 자체는 기록됐다(`first-failure-trace.txt`).

첫 준비 조건 보완에 사용한 `hasLivePeerIntent`도 완료 경계가 아니었다.
`rememberPeerIntentRoutingId`가 wire admission만으로 intent를 live로 표시하므로,
관련 class 재검증에서는 새 monitor identity를 소비한 Admit가 liveness 상태를
바꿔 상태 객체 보존 단언이 실패했다.

최종 테스트는 기존 `monitorConnectionIds`에서 선택된 transport의 endpoint pair가
등록됐는지 확인한다. 그 뒤 Hello/Admit와 readiness를 완료해 liveness 기준 상태를
잡고, 같은 descriptor의 반복 Hello/Admit를 검증한다. 이는 테스트의 두 단계를
분리한 것이다: physical 등록과 logical idempotence. Runtime 재시도·timeout·
monitor 처리·assertion은 변경하지 않았다. 최종 class 4개 test case는 모두 통과했다.

## BLOCKERS

- **전체 gate 한 번에서 0 FAIL이라는 완료 조건은 미달이다.** 요청한 전체 gate
  1회에는 위 새 테스트의 준비 조건 오류 1건이 남아 있다. 수정 후 해당 class는
  4/4 통과했고 다른 관련 41개도 통과했지만, 이를 전체 gate 0 FAIL로 바꾸어 보고하지
  않는다. `:zlink-framework-core:test contractTest`의 추가 최종 green 실행은 감독 단계에 남는다.
- 최종 focused 실행·Java 7/7 sample·Kotlin ZoneWorld에는 남은 실패가 없다.
- 완료 진단 직접 단언은 기존 trace를 켠 실행에서만 수행한다
  (`@EnabledIfEnvironmentVariable`, `ZLINK_JAVA_STREAM_TRACE=1`). 이 작업의 실행에서는
  skipped 0이다. Trace 없는 일반 실행에서는 이 진단 전용 test 하나가 skip된다.
- .NET D-097과 같이 **outbound Hello 시작**을 막는다. Seal 뒤 inbound Hello에 대한
  기존 Admit 응답 정책은 변경하지 않았다. 그 응답에는 현재 Draining descriptor가 실린다.
- 기존 descriptor fence·transport identity 수정, Core·bindings·spec·다른 언어·
  shared sample·runner는 수정하지 않았다. 요청한 요약 외 문서는 수정하지 않았고 commit도 없다.

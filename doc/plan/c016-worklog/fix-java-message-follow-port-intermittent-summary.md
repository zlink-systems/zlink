# Java: Message Follow port test 간헐 실패 — raw mesh Update가 pending physical candidate를 소비

판정: **FIXED (분류 B, 기존 결함).** `ZLinkActorClientMessageFollowRuntimePortTest`의 실패는 Message
Follow refresh와 무관하다. 실패한 assertion은 fixture의 `awaitAdmitted` (`:510`) — 세 raw mesh node의
양방향(bilateral) manual connect에서 한 peer가 2초 안에 `ADMITTED`가 되지 않았다. 원인은
`ZLinkJavaRawMeshNode`가 peer의 **Update**(post-admit-descriptor-sync) 명령을 처리할 때 pending physical
candidate(상대의 reciprocal connect가 만든 `CONNECTION_READY` edge의 identity)를 새 connection으로 설치한
것이다. Update로 설치된 connection에는 Admit가 교환되지 않으므로 `admissionControlReadyConnections`가
다시 채워질 경로가 없고, peer는 liveness가 ready가 된 뒤에도 영원히 `CONNECTING`이다.

작업 기준: `/home/hep7/project/zlink` main `6d198f05db` (Core `d8b65141a4`, java `4a76f8b489` 포함). commit 없음.
Core·bindings·spec·다른 언어 변경 없음. 증거 디렉터리:
`/tmp/claude-1000/-home-hep7-project-zlink/be443aad-d720-4c23-9860-099f913fdfd4/scratchpad/mf-repro/`
(이하 `EVID/`). 임시 runtime 로깅은 추가하지 않았다 — 기존 `ZLINK_JAVA_STREAM_TRACE=1`만 켰다.

## 실패 sequence (EVID/full-1-xml, node `runtime-port-target`, peer `runtime-port-client`)

Fixture: source·target·client 세 node가 각각 bind하고, source→client, target→client, client→source,
client→target을 모두 connect한다(각 link가 bilateral). 아래는 target이 client를 보는 trace다.

```text
target  service-received Hello(1) source=client
target  application-pair-installed command=1 connection=9018834b            # Hello → admitted identity 9018834b
target  admission-control-send admit-response → markAdmissionControlReady(9018834b)
target  monitor CONNECTION_READY conn=20 lane=0 rid=client local=  remote=inproc://client   # target→client (outbound)
        registerTransportConnection: 9018834b 재사용 (admitted, monitor 미등록)
target  monitor CONNECTION_READY conn=32 lane=0 rid=client local=inproc://target remote=    # client→target (inbound, reciprocal)
        registerTransportConnection: 새 UUID 056b3670 → pendingConnectionIds[(client, INBOUND)] += 056b3670
target  service-received Admit(2) source=client → connection 9018834b 유지, admissionControlReady[client]=9018834b
target  service-received Update(4) source=client   (client의 post-admit-descriptor-sync)
target  application-pair-installed command=4 connection=056b3670           # ← Update가 pending candidate를 소비, connection 교체
        admissionControlReadyConnections.remove(client)                     # connectionId != previous
        liveness.admit(client, 056b3670)
target  liveness-ack connection=056b3670 probe=1 accepted=false             # 이전 connection의 probe
target  liveness-ack connection=056b3670 probe=2 accepted=true              # liveness는 ready
...     이후 Hello/Admit 없음 → admissionControlReadyConnections[client] 영구 부재 → isReadyPeer=false
test    awaitAdmitted(target, CLIENT_RID) 2초 뒤 assertTrue 실패 (:510)
```

같은 실행에서 `runtime-port-source`도 pending candidate(`20e916d4`)를 가졌지만, client가 **Hello를 재전송**
(READY edge가 `nextAnnouncementNanos`를 0으로 리셋)해 그 Hello가 candidate를 소비했고 Admit 응답 →
`markAdmissionControlReady`로 ready가 됐다. target은 client의 topology에 이미 있어 재Hello가 없었다.
두 node의 차이는 "pending candidate를 소비한 명령이 Hello인가 Update인가"뿐이다.

두 번째 표본 `EVID/full-3-xml`은 같은 기전이 `runtime-port-source`에서 발생했다: Update(4)로
`39512104 → 19b8e271` 교체, `19b8e271 probe=2 accepted=true`, Admit 없음.

## 원인 (file:line, HEAD `6d198f05db`)

경로 앞부분 `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/`.

- `ZLinkJavaRawMeshNode.java:7055-7071 connectionIdForAdmission` — 모든 admission 명령에 대해 pending
  physical candidate(`:7061 pending.poll()`)를 admitted identity보다 먼저 반환했다. Update도 예외가 아니었다.
- `:6427-6440 dispatchAdmission` — Update의 direction을 현재 admitted connection의 direction으로 추론
  (`:6434`)한 뒤 그 `(peer, direction)` key로 `connectionIdForAdmission`을 호출(`:6438`)했다. Reciprocal
  connect의 READY edge는 `drainMonitorEvents:6722`에서 같은 key의 pending candidate로 등록되므로, Update가
  그것을 소비해 `topology.admit`에 새 connection으로 넘겼다(`ZLinkServiceAdmissionGuard.selectConnection`이
  같은 direction의 discriminator 문자열 비교로 교체를 허용).
- `:6491-6494` — Update로 connectionId가 바뀌면 `admissionControlReadyConnections.remove`. 이 표식을 채우는
  경로는 수신 Admit(`:6512`)과 송신 Admit 완료(`markAdmissionControlReady:6592`)뿐이다. Update 경로에는 없다.
- `isReadyPeer:1316`은 `topology.peer().connectionId == admissionControlReadyConnections.get()`과
  liveness ready를 모두 요구 → `peers()`는 `CONNECTING`을 반환.

간헐 조건: reciprocal connect의 READY edge가 pending candidate로 등록된 뒤, 그 candidate를 소비할 첫
admission 명령이 Update여야 한다(재Hello가 먼저 오면 정상). `4a76f8b489`가 pump를 "receive 진행 → monitor
drain → dispatch" 순으로 바꿔 같은 receive turn의 READY가 같은 turn의 Update dispatch보다 먼저 등록되므로
이 창이 넓어졐다; 그 이전에도 다음 Update(weight 갱신 등)가 lingering candidate를 소비할 수 있었다.
Core `d8b65141a4`(close의 동기 endpoint 해제)는 관련 없다 — 실패 trace에 admission 이전 close·DISCONNECTED·
endpoint 해제가 없다. 같은 JVM의 앞선 test class가 더 빨리 닫히는 스케줄 변화만 준다. 기존 결함(B)이다.

## 수정

| 파일 | 변경 |
|---|---|
| `ZLinkJavaRawMeshNode.java` `connectionIdForAdmission(peer, command, direction)` | pending physical candidate는 handshake 명령(Hello/Admit)만 바인딩한다. Update는 admitted connection identity(`topology.peer().connectionId`)를 그대로 쓴다. 규칙 소유자는 이 메서드 하나다. |
| 같은 파일 `dispatchAdmission` | Update의 direction을 현재 connection에서 추론하던 분기 제거(Update는 connection을 바꾸지 않으므로 사용되지 않는 값이었다). `admissionControlReadyConnections.remove` 조건은 `command != UPDATE`로 단순화(`previousConnectionId` 비교 삭제 — Update가 identity를 바꿀 수 없으므로 동치). |
| `ZLinkJavaRawMeshNodeM6ATest.java` | 기존 `connectionIdForAdmissionReusesCoreSelectedRouteAcrossCommands`를 새 signature(Hello/Admit 명령 전달)로 갱신. 신규 동기 회귀 `updateAddressesTheAdmittedConnectionAndLeavesPendingCandidatesToTheHandshake`: admitted peer + 같은 key의 pending candidate를 심고, Update → admitted identity·candidate 보존, Hello → candidate 바인딩을 검증. 수정 전 코드(guard 비활성)에서 `expected: <admitted-by-handshake> but was: <ready-edge-of-reciprocal-connect>`로 실패(`EVID/red-check.log`, `EVID/red-check-M6A.xml`). |

Fixture(`ZLinkActorClientMessageFollowRuntimePortTest`)는 변경하지 않았다. assertion·대기 예산·순서 그대로다.
새 상태·타이머·헬퍼·옵션·재시도 없음. Sleep·timeout 증가 없음.

**수정 전/후 규칙 수:** admission 명령의 connection identity 결정 3 → 1. 전(前): (a) Hello/Admit은 명령이
추론한 direction의 pending candidate 우선, (b) Update는 현재 connection의 direction을 추론해 그 pending
candidate 우선, (c) Update가 identity를 바꾸면 admission 완료 표식 제거. 후(後): pending candidate는
handshake만 바인딩하고 그 외(Update·재전송)는 admitted identity를 재사용한다 — (b)(c) 소멸.

## 소유 계층·spec 조항·교차언어·분류

- **소유 계층:** Java Framework raw mesh(`ZLinkJavaRawMeshNode`)의 logical admission — admission 명령과
  physical candidate identity의 대응. Core는 physical route 선택·RID 중복 handover(standby pipe)를 소유하며
  변경하지 않았다.
- **Spec 조항:** mesh-node §7.1 "Peer 연결" — handshake(Hello/Admit)가 candidate connection의 admission을
  정하고 중복 candidate는 ready connection 하나로 수렴; §7.2 "Channel weight 갱신" — Update(descriptor
  revision 증가)는 "connection을 다시 만들거나 application message를 다시 적용하지 않"는다. Update가 새
  connection을 설치한 것은 §7.2 위반이었다.
- **교차언어 대조:** .NET `ZLinkMeshPeerAdmission.FindForAdmission`(`ZLinkMeshPeerAdmission.cs`)은
  `command == Update`면 `peersByRid`의 기존 peer를 무조건 반환하고 candidate(`ForHandshake`)는 Hello/Admit에만
  적용한다. Node `raw-service-mesh-runtime.ts:1321 currentConnectionCandidate`는 `connectionIds`의 현재
  connection을 먼저 쓰고, 없을 때만 unresolved candidate를 promote한다. Java만 pending candidate를 모든
  명령에 우선 적용했다 — 구조적 차이가 아닌 Java 단독 결함이며, 수정으로 세 언어가 같은 규칙이 된다.
- **변경 분류:** B (기존 결함). Core/binding 보상, spec 변경, fixture 완화 없음.

## 결과

환경: `framework/languages/java`, `TMPDIR=/dev/shm/zlink-tmp-java`, `unset ZLINK_LIBRARY_PATH`,
`ZLINK_JAVA_STREAM_TRACE=1`, 모든 gradle 호출 `flock -w7200 /tmp/zlink-jvm-gate.lock ./gradlew --no-daemon …`.

| 항목 | 결과 | 증거 |
|---|---|---|
| 수정 전 baseline: 전체 `:zlink-framework-core:test --rerun` ×5 | 1256 tests: MessageFollow 실패 **2/5** (iter 1, 3; 둘 다 `:510`, trace에 Update connection 교체); `ZLinkJavaRawMeshNodeShutdownSealTest` 실패 2/5 (iter 2 `crossedHelloAdmit…`, iter 3 `unsealedPeerReadmits…[1] false`); `ZLinkServiceOperationRegistryTest.cancellationAtomicallyTakesTheOperationBeforeClose` 실패 1/5 (iter 1) | `EVID/full-loop.status`, `EVID/full-{1..5}.log`, `EVID/full-{1..5}-xml/` |
| 수정 후 focused (M6A 29 + MessageFollow 1) | 30/30 PASS | `EVID/focused-1.log` |
| 새 회귀, guard 비활성(red) | FAIL `expected: <admitted-by-handshake> but was: <ready-edge-of-reciprocal-connect>` | `EVID/red-check.log`, `EVID/red-check-M6A.xml` |
| 수정 후 class ×10 (`--tests …MessageFollowRuntimePortTest --rerun`, trace on) | **10/10 PASS** (class 시간 0.659–0.693 s) | `EVID/verify.status`, `EVID/class-{1..10}.log`, `EVID/class-{1..10}-xml/` |
| 수정 후 전체 gate 1 `:zlink-framework-core:test --rerun contractTest --continue` | Gradle 1257 tests 완료(기존 1256 + 새 회귀 1), 실패 0; leaf XML core **1244/1244**, contract **96/96**; `BUILD SUCCESSFUL` 48 s | `EVID/gate-1.log`, `EVID/gate-1-xml/` |
| 전체 gate 2 (스크립트: `:zlink-framework-core:test contractTest --continue --rerun`) | **무효** — `--rerun`은 바로 앞 task(`contractTest`)에만 붙어 `:zlink-framework-core:test`가 UP-TO-DATE로 실행되지 않았다(11 s). 집계에 넣지 않음. | `EVID/gate-2.log` |
| 전체 gate 2b `:zlink-framework-core:test --rerun contractTest --rerun --continue` | 1257 tests 완료, **1 실패**: `ZLinkJavaRawMeshNodeShutdownSealTest.crossedHelloAdmitUsesCompletionDiagnosticOnBothSidesWithoutResettingLiveness` `:158`(수정 전 baseline iter 2와 같은 test·같은 await). MessageFollow PASS. contract 96/96. 아래 "별도 관찰" 참고 | `EVID/gate-2b.log`, `EVID/gate-2b-xml/` |
| 전체 gate 3 (같은 명령) | 1257 tests, 실패 0; leaf XML core **1244/1244**, contract **96/96**; `BUILD SUCCESSFUL` 48 s | `EVID/gate-3.log`, `EVID/gate-3-xml/` |
| `git diff --check` | clean | — |

## BLOCKERS / 별도 관찰

- **BLOCKER 없음** (이 test·이 기전 기준).
- 요구된 "전체 gate 2회 0 실패"는 gate 1·gate 3으로 충족했다. 그 사이의 gate 2b는 수정 전 baseline에도
  2/5로 나타난 `ZLinkJavaRawMeshNodeShutdownSealTest`의 기존 간헐 실패 1건만 담았다.
- 별도 결함 후보(수정하지 않음, 이번 brief 범위 외 — 다른 test의 첫 실패로 분리 보고):
  - `ZLinkJavaRawMeshNodeShutdownSealTest.crossedHelloAdmitUsesCompletionDiagnosticOnBothSidesWithoutResettingLiveness`
    `:158` (`EVID/full-2-xml/`, `EVID/gate-2b-xml/`; `ZLINK_JAVA_STREAM_TRACE=1`일 때만 활성화되는 test),
    `unsealedPeerReadmitsIncludingRelocationDraining[1] false` `:112` (`EVID/full-3-xml/`). 세 trace 모두
    Update에 의한 connection 교체(`command=4`에서 connection 변화)가 없다 — 이 수정의 기전이 아니다. crossed
    trace에는 Core reciprocal handover의 anonymous hex RID standby pipe READY(`rid=hex:…`)와 test가 주입한
    반복 Hello/Admit 사이클이 보이며, `:158`의 await는 `completed(Admit)`과 `isReadyPeer` 양쪽을 함께 요구한다.
    별도 job 권고.
  - `ZLinkServiceOperationRegistryTest.cancellationAtomicallyTakesTheOperationBeforeClose` `:109`
    (`EVID/full-1-xml/`): raw mesh와 무관한 registry 단위 test. 이전 worklog
    (`fix-java-deliverydispatch-courier-session-teardown-summary.md`)에도 간헐 보고가 있다. 별도 job 권고.
- 관찰: `ZLinkJavaRawMeshNode`는 같은 fact(peer의 selected connection id)를 `topology.peer().connectionId`와
  `connectionIds` 두 곳에 유지한다(기존). 이번 수정은 그 중복을 늘리지 않았고(`previousConnectionId` 비교 제거),
  통합은 별도 refactor checkpoint 후보로 남긴다.

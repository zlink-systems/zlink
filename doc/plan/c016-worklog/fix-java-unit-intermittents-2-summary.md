# Java: unit suite 잔여 간헐 실패 2건 — seal test(readmit·crossed)와 operation registry cancellation

판정: 세 failure mode 모두 원인 확정·수정. (1a)·(1b)는 **runtime 결함(분류 B)**, (2)는 **fixture 결함**
(runtime은 spec 01 §11대로 동작). Core 원인 없음 — trace의 anonymous hex RID standby-pipe READY는
reciprocal connect의 정상 handover이며 어느 실패 기전에도 관여하지 않는다(BLOCKER 없음).

작업 기준: `/home/hep7/project/zlink` main `f03e6aae99` (Core `d8b65141a4`, java `4a76f8b489`·`9d6e4b6299` 포함).
commit 없음. Core·bindings·spec·다른 언어 변경 없음. 증거 디렉터리:
`/tmp/claude-1000/-home-hep7-project-zlink/be443aad-d720-4c23-9860-099f913fdfd4/scratchpad/ui2/` (이하 `EVID/`),
이전 agent 증거 `…/scratchpad/mf-repro/` (이하 `PREV/`). 임시 runtime 로깅 없음 — 기존 `ZLINK_JAVA_STREAM_TRACE=1`만 사용.

환경: `framework/languages/java`, `TMPDIR=/dev/shm/zlink-tmp-java`, `unset ZLINK_LIBRARY_PATH`, 모든 gradle 호출
`flock -w7200 /tmp/zlink-jvm-gate.lock ./gradlew --no-daemon …`. class 반복은 CPU busy-loop 12개 병행(`EVID/repro/loop.sh`).

## 재현 (수정 전)

| 항목 | 결과 | 증거 |
|---|---|---|
| `ZLinkJavaRawMeshNodeShutdownSealTest` ×10, trace on, CPU 부하 | **3/10 실패**: iter 4·5 `unsealedPeerReadmitsIncludingRelocationDraining[1]` (`:112`), iter 8 `crossedHelloAdmit…` (`:158`) | `EVID/repro/seal-trace.status`, `EVID/repro/seal-trace-{4,5,8}-xml/` |
| `ZLinkServiceOperationRegistryTest` ×10, CPU 부하 | 10/10 pass (class 단위로는 재현 안 됨) | `EVID/repro/registry.status` |
| registry fixture 순서를 컴파일된 runtime class에 대해 20,000회 반복 (`EVID/RegistryRace.java`) | `callbacks==0` **4/20000** | `EVID/RegistryRace.java` 출력 |
| 순수 JDK: `whenComplete` 등록 → 다른 thread에서 `cancel` → `join()` 복귀 시 callback 미실행 | **90/20000** | `EVID/JoinRace.java` |

## (1a) `unsealedPeerReadmitsIncludingRelocationDraining[1]` `:112`

실제 실패는 `assertTrue`가 아니라 `replacePeerConnection`이 던진
`IllegalStateException: previous peer connection has not completed liveness close`
(`ZLinkJavaRawMeshNode.java:947`; `PREV/full-3-xml`, `EVID/repro/seal-trace-{4,5}-xml`). 이전 요약의 "expected true was
false"는 `[1]`이 아니라 crossed 쪽 문구가 섞인 것이다.

### Sequence (`EVID/repro/seal-trace-4-xml`, node `unsealed-local`)

```text
test    peer.close()
local   monitor-event DISCONNECTED connection=29 lane=0 rid=unsealed-peer
local   drainMonitorEvents → cleanupTerminalTransport → disconnectAdmitted(peer, conn)
          topology.disconnect, admissionControlReady.remove, admittedPeerChannels.remove,
          disconnectedPeers.add(peer)                          # ← peers()가 이 순간부터 CLOSED(≠ADMITTED)
test    await(!admitted(local)) 통과 → replacePeerConnection(...)
local   peerIntentIsClosed(1) == false (closedPeerIntents 아직 비어 있음) → requestPeerIntentClose, throw  (:947)
local   markPeerIntentsClosed → peer-intent-closed intent=1 admittedClosed=true → closedPeerIntents.add(1)   # 한 pump turn 뒤
```

같은 pump turn 안에서 "연결 끊김"이 두 상태에 두 시점으로 게시된다: `disconnectedPeers`(public `peers()`의 `CLOSED`)가
먼저, `closedPeerIntents`(`replacePeerConnection`의 허용 조건)가 나중. 그 사이에 caller thread가 `CLOSED`/`!ADMITTED`를
보고 replace를 호출하면 거부된다. 기존 fixture들은 이미 이 창을 우회하고 있었다 — `ZLinkJavaRawMeshNodeM6ATest.awaitReplacement`는
`IllegalStateException`을 2초 동안 재시도하고, `ZLinkJavaRawMeshNodeTransportIdentityTest:273`은 `CLOSED` 뒤에
`!hasLivePeerIntent`를 한 번 더 기다린다.

### 원인 (file:line, HEAD `f03e6aae99`)

- `ZLinkJavaRawMeshNode.java:156 disconnectedPeers` — 유일한 reader가 `peers():1132`의 `CLOSED` 판정이다. 같은 사실("이
  intent의 admitted connection이 끝났다")을 `closedPeerIntents:160`이 따로 보유하며 `:6825`에서 나중에 게시한다.
- `:7014-7017 disconnectAdmitted` — `disconnectedPeers.add`가 `markPeerIntentsClosed:6825 closedPeerIntents.add`보다 먼저 실행된다.
- fixture `ZLinkJavaRawMeshNodeShutdownSealTest:55,:107` — `await(!admitted(local))`은 `CONNECTING`으로도 만족된다.
  mesh-node §7.1 (3)은 재연결 허용 조건을 "liveness 확인으로 이전 pipe 종료 확정"으로 두므로, 기다려야 하는 관측은
  `CLOSED`다(`CONNECTING`은 D-094대로 Core connect intent가 살아 있는 상태로, replace가 거부되는 것이 맞다).

### 수정

| 파일 | 변경 |
|---|---|
| `ZLinkJavaRawMeshNode.java` | `disconnectedPeers` 집합과 그 add/remove 6곳 삭제. `peers()`의 `CLOSED`는 `closedPeerIntents.contains(intentId)` 하나에서 나온다 — replace 허용과 같은 게시. |
| `ZLinkJavaRawMeshNodeShutdownSealTest.java:55,:107` | `await(!admitted)` → `await(closed(local))` (`CLOSED` 관측). 대기 예산·assertion 불변. |
| `ZLinkJavaRawMeshNodeTransportIdentityTest.java` 신규 `closedIsPublishedExactlyWhenReplacementBecomesEligible` | 기존 helper(동기 monitor handler 구동)로 admitted 상태를 심고 monitor drain 순서대로 `cleanupTerminalTransport` → `markPeerIntentsClosed`를 호출한다. 첫 단계 직후 `peers()==CLOSED`와 `peerIntentIsClosed`가 같아야 한다. HEAD runtime에서 `CLOSED published before replacement eligibility ==> expected: <false> but was: <true>`로 실패(`EVID/red-check.log`). |

**수정 전/후 규칙 수:** "peer의 연결이 끝났다"의 상태 2(`disconnectedPeers`, `closedPeerIntents`) → 1. `CLOSED` 게시와 replace
허용이 다른 시점에 일어나는 경로가 사라졌다. 새 상태·타이머·헬퍼·재시도 없음.

## (1b) `crossedHelloAdmitUsesCompletionDiagnosticOnBothSidesWithoutResettingLiveness` `:158`

세 표본(`PREV/full-2-xml`, `PREV/gate-2b-xml`, `EVID/repro/seal-trace-8-xml`) 모두 test 소요 0.026–0.617 s — await 예산
5 s의 만료가 아니다. fixture `await`는 조건이 true가 되면 loop를 벗어난 뒤 **다시 평가**해 assert하므로, 실패는
"수렴 안 함"이 아니라 조건의 **true → false 반전**이다. `completed(records, …)`는 단조 증가하므로 반전 가능한 항은
`ready(left/right)` = `isReadyPeer`뿐이다.

### Sequence (`PREV/full-2-xml`)

```text
test    sendAdmission(left→right, Hello), (right→left, Hello)           # 1라운드
right   service-received Hello → application-pair-installed connection=4955cb94 (같은 selected connection)
          admissionControlReadyConnections.remove(left)      :6483      # ← 여기서 isReadyPeer(left)=false
          … liveness.admit(같은 connection → no-op) …
          admit-response send → 완료 시 markAdmissionControlReady.put(left, 4955cb94)
left    (대칭)
both    Admit 수신(runtime admit-response) → remove(:6483) … put(:6503)  → await 조건 true 관측
both    service-received command=2 (test가 주입한 Admit) → remove(:6483) … put(:6503)
test    await의 최종 재평가가 remove~put 창에 걸림 → ready()=false → "condition did not converge"
```

`hex:…` RID의 READY(conn 523)는 right→left reciprocal connect의 Core standby pipe다. 그 candidate는 hex RID key로만
등록되어 어떤 Hello/Admit에도 바인딩되지 않았고(모든 `application-pair-installed`가 원래 connection id), 실패 기전에 관여하지 않는다.

### 원인 (file:line, HEAD `f03e6aae99`)

- `ZLinkJavaRawMeshNode.java:6482-6484 dispatchAdmission` — Hello/Admit이면 `admissionControlReadyConnections.remove(source)`.
  같은 selected connection에 대한 **idempotent** 재Hello/재Admit(`topology.admit`이 `admitDescriptorUpdate`로 `ADMITTED` 반환)에도
  표식을 지운 뒤 `:6503`(Admit 수신) 또는 `markAdmissionControlReady:6592`(Admit 송신 완료)에서 다시 넣는다.
- `isReadyPeer:1316`은 `topology.peer().connectionId == marker && liveness.isReady`. 이 표식은 connection id로 비교되므로
  connection이 바뀌면 옛 표식은 **비교에서 자동으로 무효**다 — remove가 필요한 경우가 없다. 반대로 connection이 같으면 remove는
  readiness를 잠시 거짓으로 만든다. `sendChannel:1578`은 caller thread에서 `selectChannel(this::isReadyPeer)`를 평가하므로
  이 창은 application send에도 보인다("ready peer 없음").
- liveness는 무관: `ZLinkServiceLivenessRegistry.admitCore:82`는 같은 connection이면 return(epoch 보존) — test의 `assertSame(epoch)`가 통과하는 이유.

### 수정

| 파일 | 변경 |
|---|---|
| `ZLinkJavaRawMeshNode.java:6482-6484` | handshake 시 `admissionControlReadyConnections.remove` 삭제. 표식은 "Admit 교환이 완료된 connection id"이며, selected connection과의 동일성 비교가 유효성을 결정한다. 설정 경로(Admit 수신·Admit 송신 완료)와 해제 경로(`disconnectAdmitted`의 조건부 remove, intent 제거, not-required)는 그대로다. |
| `ZLinkJavaRawMeshNodeShutdownSealTest.java` 신규 `repeatedHelloOrAdmitOnTheSelectedConnectionNeverClearsAdmissionReadiness` (env 무관) | admitted pair를 만든 뒤 `admissionControlReadyConnections`를 기록형 map으로 교체하고 같은 descriptor로 Hello·Admit·Hello를 재전송한다. 각 명령의 완료(재mark)를 기다린 뒤 clear 0회와 `isReadyPeer` 유지를 검증한다. 한 dispatch turn 안의 창은 외부 sampling으로 결정론적으로 잡을 수 없어 표식 자체를 관측한다. HEAD runtime에서 `command 1 cleared admission readiness ==> expected: <0> but was: <1>`로 실패(`EVID/red-check.log`). |

fixture(`crossedHelloAdmit…`)는 변경하지 않았다.

**수정 전/후 규칙 수:** 표식 결정 2(handshake가 지운다 + Admit 완료가 넣는다) → 1(Admit 완료가 넣고, connection 동일성 비교가
유효성을 정한다). "이 경우만" 분기 없음.

## (2) `ZLinkServiceOperationRegistryTest.cancellationAtomicallyTakesTheOperationBeforeClose` `:109`

### Sequence

```text
test    operation.completion().whenComplete(cb)          # dependent stage 등록
test    completion.cancel(false) → registry.cancel → take → completions.post(entry)   # terminal은 completion lane에 게시
lane    entry.dispatch → completion.completeCancellation → CompletableFuture.postComplete
          dependent stack(LIFO): [Signaller(join), UniWhenComplete(cb)] → Signaller 먼저 → test thread unpark
test    join() 복귀 → registry.close() → callbacks.get() == 0            # cb는 lane thread가 아직 실행 중/전
lane    UniWhenComplete → cb 실행 → 1
```

### 원인

fixture `ZLinkServiceOperationRegistryTest.java:102-109`. `join()`은 source stage의 완료만 보장하고 dependent action의 실행은
보장하지 않는다(`CompletableFuture` 계약; `EVID/JoinRace.java` 90/20000, runtime class 기준 `EVID/RegistryRace.java` 4/20000).
runtime은 spec 01-submit-and-completion §11 "완료 callback은 process 공유 completion dispatcher에 넣고 새 execution turn에서
실행"과 §10 "atomic take"를 그대로 따른다(`ZLinkServiceOperationRegistry.cancel:186-197`, `ZLinkServiceCompletionDispatcher`).
`close()`가 lane을 기다리게 하는 것은 §11의 turn 분리를 깨므로 runtime 변경 대상이 아니다.

### 수정

| 파일 | 변경 |
|---|---|
| `ZLinkServiceOperationRegistryTest.java:102-115` | `whenComplete`가 반환하는 dependent stage를 보관하고, source `join()`(CancellationException) 뒤에 dependent `join()`(`CompletionException` cause `CancellationException`)을 확인한다. dependent stage의 완료가 callback 실행 뒤임은 `CompletionStage` 계약이다. 그 다음 `registry.close()` → `callbacks==1` → late `complete==false` 순서는 그대로다. |

대기 예산 없음(여전히 timeout 없는 `join`). 고친 순서로 `EVID/RegistryRaceFixed.java` 0/20000.

**수정 전/후 규칙 수:** runtime 변경 없음. fixture의 완료 관측 지점 2(source join + 암묵적 "callback도 끝났을 것") → 1(dependent stage).

## 소유 계층·spec 조항·교차언어·분류 (runtime 변경 (1a)(1b))

- **소유 계층:** Java Framework raw mesh(`ZLinkJavaRawMeshNode`)의 logical admission 상태 게시 — peer readiness 표식과 manual
  intent의 closed 게시. Core(물리 route·RID 중복 handover·standby pipe)와 binding은 관여하지 않으며 변경하지 않았다.
- **Spec 조항:** mesh-node §7.1 — 같은 selected connection의 재Hello/재Admit은 idempotent admission이며 ready connection
  하나로 수렴(1b); §7.1 manual 재연결 조건 (3) "liveness 확인으로 이전 pipe 종료 확정"이 replace 허용의 유일한 근거이고
  그 public 관측이 `CLOSED`(runtime-monitoring `not_connected`)다(1a). 05-host-relocation-flow §14 step 1(D-097)의 seal
  규칙은 `[2]`(draining) 변형의 assertion에 해당하며 이번 실패와 무관. 01-submit-and-completion §10·§11은 (2)의 runtime 판정 근거.
- **교차언어 대조:** .NET `ZLinkManagedMeshNode.cs:8310-8347` — Idempotent 결정은 "changes no peer state"로 처리하고 완료
  diagnostic만 남긴다; readiness 표식을 지우고 다시 넣는 단계가 없다. Java만 clear→re-mark를 했다(1b, Java 단독 결함).
  (1a) `replacePeerConnection`과 closed-intent 게이트는 Java 전용 표면이다(.NET은 intent 제거 시에만 `Closed`, 연결 종료 시
  `Connecting`으로 두고 새 generation Hello로 재admit; Node는 해당 API 없음). Java 안에서 같은 사실을 두 상태로 게시한 것이
  결함이며 구조적 언어 차이가 아니다. (2) 다른 언어 fixture와 무관(JDK `CompletableFuture` 의미).
- **변경 분류:** (1a) B, (1b) B — 기존 결함. (2) fixture 결함(runtime 분류 대상 아님). C/D 없음.

## 결과

| 항목 | 결과 | 증거 |
|---|---|---|
| 수정 후 focused 4 class (Seal 5 · TransportIdentity 11 · M6A 29 · Registry 10), trace on | **55/55 PASS** | `EVID/focused-1.log`, `EVID/focused-1-xml/` |
| 새 회귀 2건, HEAD runtime(red) | Seal `repeatedHelloOrAdmit…` FAIL `command 1 cleared admission readiness expected <0> but was <1>`; TransportIdentity `closedIsPublishedExactly…` FAIL `CLOSED published before replacement eligibility expected <false> but was <true>`; 수정 runtime 복원 확인(`cmp`) | `EVID/red-check.log`, `EVID/red-check-xml/` |
| `ZLinkJavaRawMeshNodeShutdownSealTest` ×10, trace **on**, CPU 부하 | **10/10 PASS** (5 tests × 10 = 50/50, class 1.33–1.48 s) | `EVID/repro/v-seal-trace.status`, `v-seal-trace-{1..10}-xml/` |
| 같은 class ×10, trace **off**, CPU 부하 (crossed test는 skip, 4 tests) | **10/10 PASS** (40/40 + skipped 10) | `EVID/repro/v-seal-notrace.status`, `v-seal-notrace-{1..10}-xml/` |
| `ZLinkServiceOperationRegistryTest` ×10, CPU 부하 | **10/10 PASS** (100/100) | `EVID/repro/v-registry.status`, `v-registry-{1..10}-xml/` |
| `ZLinkJavaRawMeshNodeTransportIdentityTest` ×10, trace on, CPU 부하 | **10/10 PASS** (110/110) | `EVID/repro/v-transport.status`, `v-transport-{1..10}-xml/` |
| 전체 gate 1 `:zlink-framework-core:test --rerun contractTest --rerun --continue` (trace on) | core **1259/1259**(기존 1257 + 새 회귀 2), core contractTest **27/27**, `BUILD SUCCESSFUL` 48 s | `EVID/repro/gate-1.log`, `gate-1-xml/` |
| 전체 gate 2 (같은 명령) | core 1259 중 **1 실패**: `ZLinkManualFanoutRuntimeOwnerTest.connectionIsNotReceivableBeforeConnectCommit` `:51` — `TimeoutException` at `Fixture.awaitInfrastructureIdle:205`(controlled backend fixture의 1 s executor idle 대기). raw mesh node·operation registry를 참조하지 않는 class(변경 class import 0). Seal·Registry·TransportIdentity·M6A 모두 PASS. contract 27/27. 아래 "별도 관찰" | `EVID/repro/gate-2.log`, `gate-2-xml/` |
| 전체 gate 3 (같은 명령) | core **1259/1259**, contract **27/27**, `BUILD SUCCESSFUL` 48 s | `EVID/repro/gate-3.log`, `gate-3-xml/` |
| `ZLinkManualFanoutRuntimeOwnerTest` 단독 ×6, CPU 부하 (gate 2 실패의 독립성 확인) | **1/6 실패**, 같은 test·같은 `:205` TimeoutException — 이번 변경과 무관한 기존 간헐 | `EVID/repro/fanout.status`, `fanout-3-xml/` |
| `git diff --check` | clean | — |

요구된 "전체 gate 2회 0 실패"는 gate 1·gate 3으로 충족했다. 그 사이 gate 2의 유일한 실패는 변경 class와 무관한 fanout fixture의
기존 간헐(단독 반복에서도 1/6 재현)이다. contract 집계는 `zlink-framework-core/build/test-results/contractTest`(4 class 27 tests)
기준이다 — 다른 module의 contractTest는 `NO-SOURCE`.

## BLOCKERS / 별도 관찰

- **BLOCKER 없음.** Core 원인 없음 — anonymous hex RID standby-pipe READY는 reciprocal connect의 정상 handover로, 세 trace
  모두에서 어떤 admission 명령에도 바인딩되지 않았다.
- 별도 결함 후보(수정하지 않음, brief 범위 외 — 첫 실패로 분리 보고):
  `ZLinkManualFanoutRuntimeOwnerTest.connectionIsNotReceivableBeforeConnectCommit` `:51` →
  `Fixture.awaitInfrastructureIdle:205` `infrastructure.submit(() -> {}).get(1, SECONDS)` TimeoutException. gate 2에서 1회,
  단독 ×6(CPU 부하)에서 1회. 변경 class를 참조하지 않는 controlled-backend fixture다. 별도 job 권고(`EVID/repro/gate-2-xml/`,
  `EVID/repro/fanout-3-xml/`).
- `ZLinkJavaRawMeshNodeM6ATest.awaitReplacement`(`:1494` 부근)의 2초 `IllegalStateException` 재시도 loop와
  `ZLinkJavaRawMeshNodeTransportIdentityTest:273-274`의 `CLOSED` + `!hasLivePeerIntent` 이중 대기는 (1a)의 창을 우회하던 기존
  fixture다. 이번 수정으로 `CLOSED` 한 번의 관측으로 충분해졌다 — 정리는 별도 refactor checkpoint 후보(동작 변경 없음, 이번 brief 범위 외).
- 관찰(변경 없음): closed intent에 대해서도 `nextAnnouncementNanos`가 0으로 리셋되어 `mesh_peer_admission_sent command=Hello`가
  한 번 나간다(`EVID/repro/seal-trace-4-xml` 끝부분). seal test의 `sealed` 변형은 gate로 이를 막는다; unsealed에서는 무해하나
  retire된 endpoint에 대한 불필요한 송신이다. 별도 항목.
- 관찰(변경 없음, 이전 요약과 동일): selected connection id가 `topology.peer().connectionId`와 `connectionIds` 두 곳에 있다.

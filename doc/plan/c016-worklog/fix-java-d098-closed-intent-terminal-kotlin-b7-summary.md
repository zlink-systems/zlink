# Java D-098: closed intent terminal과 Kotlin ZoneWorld B7

Part 1의 승인된 B 수정과 요청한 반복 검증을 완료했다. Kotlin ZoneWorld 전체 2회와
추가 B7 단독 3회, Java ZoneWorld 전체 1회는 통과했다. 다만 이전 B7의 JoinSpot
최하위 예외는 재현되지 않아 Part 2의 원인 확정은 BLOCKED다. 기존 미커밋 Java 변경을
보존했으며 commit과 package 재빌드는 하지 않았다.

## Part 1 — closed intent terminal

### 순서와 원인

이전 보고서의 `/tmp/zlink-java-d098/m6a-first-failure.trace`는 다음 순서다.

```text
READY 326 → intent 1 close 요청 → DISCONNECTED 326
→ endpoint retirement → closedPeerIntents에 종료 게시
→ 늦은 READY 332 → endpoint/RID로 intent 1 재귀속 → 종료 게시 취소
→ DISCONNECTED 332 → 최초 종료 조건이 없어 replacement 자격 복구 실패
```

원인은 `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:6785`의
READY 귀속 조건이다. 수정 전에는 endpoint/RID 일치만 검사하고 `:6793`에서
`closedPeerIntents.remove(intentId)`를 실행했다. 종료된 intent에 transport와 live 상태가
다시 생겨, 이미 종료를 관찰한 호출자의 replacement가 거부됐다.

### 수정과 소유권

`markPeerIntentsActive`는 기존 `closedPeerIntents`에 있는 intent를 transport 귀속 전에
제외한다. READY가 종료 사실을 제거하는 코드도 삭제했다. Close 요청 중인 아직 종료하지
않은 intent의 READY는 기존대로 기록하므로 그 transport의 종료를 관찰할 수 있다.

새 상태나 monitor generation을 두는 안과 기존 terminal 사실을 사용하는 안 중 후자를
택했다. `cleanupClosedPeerEndpoint`(`:6848`)가 Core connect intent를 먼저 retire한 뒤
`markPeerIntentsClosed`(`:6844`)가 closed를 게시하는 순서를 유지한다.

늦은 READY가 closed intent에만 일치하면 그 intent의 transport가 아니다. 기존 monitor
drain은 READY의 transport 진단 등록을 유지하고, 뒤따른 DISCONNECTED/CLOSED를
`cleanupTerminalTransport`(`:6763`) → `removeTransportConnection`(`:7234`) →
`discardPendingConnectionId`(`:7247`)로 정리한다. Admitted connection에 해당하지 않으면
`disconnectAdmitted`가 false를 반환하며 intent 종료 사실은 바뀌지 않는다. 물리 close는
이미 수행한 endpoint retirement가 소유한다. 이 수정은 READY마다 새 disconnect를
발행하거나 별도 reconnect 정책을 추가하지 않는다.

- **소유 계층:** Framework raw mesh가 logical peer intent의 terminal 상태를 소유한다. Physical close와 connect intent retirement는 Core 소유다.
- **Spec 조항:** `03-spot-actor/03-mesh-node.ko.md` §7.1(3), `02-channel-transport/05-transport-liveness.ko.md` §5, D-098 item 8(B 승인). D-098 item 3·5·6의 기존 diff를 유지한다.
- **교차언어 대조:** .NET `ZLinkMeshPeerAdmission.cs:26`의 `FindReadyOutboundCandidate`도 `MeshPeerState.Closed`를 제외하며 `ZLinkManagedMeshNode.cs:8594`가 이를 사용한다. Kotlin은 Java runtime을 공유한다. Java의 별도 closed set을 READY가 지우던 기존 결함이므로 Java만 수정한다.
- **변경 분류:** B — 감독이 승인한 기존 결함 수정. 새 API·계약 변경 없음.

**수정 전/후 규칙 수:** 종료는 terminal / READY는 종료를 취소할 수 있다는 충돌 규칙 2 →
종료는 terminal 1. 상태 소유자 1 → 1, 추가 상태·timer·retry 0.

### 회귀와 결과

`ZLinkJavaRawMeshNodeTransportIdentityTest.java:143,148`에 requested close와 admitted close의
결정적 회귀를 추가했다. 실제 monitor handler를 동기 호출하여 종료 후 endpoint만 일치하는
READY와 RID만 일치하는 READY를 각각 주입한다. Closed 유지, live/transport 귀속 부재,
public `replacePeerConnection`의 1회 성공과 새 intent의 READY 활성화를 확인한다.
Native scheduling을 배제하기 위한 router proxy는 기존 test 방식을 사용하며 connect 외의
호출은 실패시킨다. 수정 전 두 회귀 모두 closed assertion에서 실패했다.

증거 root는 `/tmp/zlink-java-d098-terminal/`이다. XML 합계는 leaf testcase 기준이다.

| 검증 | 결과 | 증거 |
|---|---|---|
| 수정 전 결정적 회귀 | 예상대로 2 FAIL | `regression-before.log`, `regression-before-xml/` |
| 수정 후 TransportIdentity focused | 13 PASS | `focused.log`, `focused-xml/` |
| M6A ×10 | 290 PASS, 0 FAIL | `repeat-{1..10}.log`, 각 `-xml/` |
| TransportIdentity ×10 | 130 PASS, 0 FAIL | 같은 반복 로그·XML |
| ShutdownSeal ×10, trace on/off 교대 | 75 PASS, 0 FAIL, 5 conditional skip | 같은 반복 로그·XML |
| 전체 core `:test` ×2 | 각 1,253 PASS, 0 FAIL, 0 skip | `gate-{1,2}.log`, 각 `-xml/` |
| `contractTest` ×2 | 각 96 PASS, 0 FAIL, 0 skip | 같은 gate 로그·XML |

ShutdownSeal의 skip은 `crossedHelloAdmitUsesCompletionDiagnosticOnBothSidesWithoutResettingLiveness`의
기존 `@EnabledIfEnvironmentVariable` 조건이다. Trace-on 5회에서는 8개 모두 실행하고,
trace-off 5회에서는 진단 전용 1개만 skip하며 나머지 7개를 실행했다.

Gate 명령은 `./gradlew --no-daemon :zlink-framework-core:test --rerun contractTest --rerun --continue`다.
두 gate 로그 모두 `:22`에 `> Task :zlink-framework-core:test`가 있다. `--rerun`을 각 task에
지정했으므로 `:test`가 UP-TO-DATE로 생략되지 않았다.

## Part 2 — Kotlin ZoneWorld B7

### 관찰 순서와 client timeout

이전 실행은 `/dev/shm/zlink-tmp-java/tmp.mr8E3oTuf3/`다. Main ledger
`logs/runner.log:6`은 `ZW-B7 failed`, `logs/client.log:13`은 `TimeoutException`을 기록한다.
다른 시나리오의 실패로 간주하거나 baseline으로 제외하지 않았다.

Actor `b7-0f4733`의 돌아오는 이동은 다음 순서다.

1. Gateway `logs/gateway.log:2784-2791`에서 MoveMsg를 수신·dispatch하고 actor로 보낸다.
2. `logs/zone-node-1.log:25291-25296`에서 같은 MoveMsg를 정상 처리한다.
3. `:25303`, `03:44:43.768`에 `JoinSpot spot=zone-nw`를 보낸다. Flow는
   `01a072e3-2473-786d-9e4b-31fd6d3395ae`다.
4. `:25480-25481`, `03:44:43.810`에 같은 flow의 `handler_exception`과
   `join completion ... Failed kind=INTERNAL_FAILURE`가 기록된다. 약 42ms 뒤 실패한
   것이므로 Join deadline까지 대기한 실패와는 구분된다. `:25360,25373`의 같은 target
   request 완료는 성공이지만 actor/correlation이 없어 이를 B7의 특정 request와
   일대일로 연결할 수는 없다.
5. Kotlin `ScenarioSupport.kt:98-125`는 zone 경계 이동 시 `ZoneChangedNotify`를 기다린다.
   `ZoneDomain.kt:150-163`은 Join 실패 시 `MoveRejectedNotify`를 보내는 경로이고,
   이 알림은 해당 waiter의 성공 조건이 아니다. 따라서 Join 실패 후 이동 완료 알림이
   오지 않으면 `ScenarioSupport.kt:20`의 기존 30초 예산에서 client timeout이 발생한다.
   실패 콜백은 종료됐지만 client의 성공 조건은 충족되지 않은 순서다.

위 Kotlin 파일의 공통 앞부분은
`framework/languages/java/samples/kotlin/ZoneWorld/`이며 client 파일은
`Client/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/client/`, server 파일은
`Server/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/server/zone/` 아래에 있다.

실패 전 zone-node-1·zone-node-2·gateway의 `peer-intent-closed` 기록은 모두 0건이다.
따라서 item 8 변경으로 B7이 고쳐졌다고 설명할 근거가 없다. B7은 main runner의 host
종료·재시작 단계보다 먼저 실행된다. 이전 로그의 이 세 role에는
`ZLink runtime termination` 원인 이벤트가 없고, 실패 후에도 actor 메시지를 처리한다.
종료 원인 이벤트를 확인한 범위와 flow 발췌는 `prior-b7-evidence.txt`,
`prior-b7-correlation.txt`에 보존했다. 이 증거만으로 item 3 seal, admission ordering,
Core 또는 환경을 원인으로 지목하지 않는다.

### 최하위 원인과 진단의 한계

**JoinSpot 최초 throw의 file:line은 미확정이다.** 확인한 예외 전달 지점은
`ZLinkActorSpotJoinCall.java:332`의 `traceJoinFailed(kind, cause)`와 `:493-510`의
flow event 생성이다. 해당 파일은 Java core의 `runtime/actors/` 아래에 있다.
예외 type/message는 event에 담기지만 `runtime/diagnostics/ZLinkTraceFormat.java:31-39`는
reason/action만 출력하고 errorType/errorMessage를 출력하지 않는다. 원래 보존 로그에는
최하위 예외 상세가 없어 사후 복구할 수 없다.

기존 message-flow와 file log를 먼저 조사한 뒤, sample 재현 동안에만 이 formatter에
기존 event의 오류 필드를 출력하는 4줄을 임시 추가했다. Flow gate 뒤의 출력만 바꾸며
admission·relocation·timeout 동작은 바꾸지 않았다. 재현 로그에서 planned node 종료에
따른 `UNAVAILABLE` 예외 상세가 출력되는 것을 확인했지만 B7의 `INTERNAL_FAILURE`는
재현되지 않았다. 임시 수정은 제거했고 patch는 `diagnostic-projection.patch`에 보존했다.
최종 `:zlink-framework-core:jar` 빌드도 성공했다(`final-jar.log`).

**Owner 판정:** (a) Java runtime / (b) Kotlin sample / (c) 환경 중 하나로 확정하지
못했다. Client timeout은 앞선 Join 실패 뒤의 결과이며, client waiter나 timeout을
바꾸는 것은 Join 실패 원인 수정이 아니다. Part 2의 runtime·sample 수정은 없다.

- **소유 계층:** Framework Actor Join/relocation이 Join의 terminal completion을 소유하고 sample client가 ZoneChangedNotify 대기를 소유한다. 최초 실패의 소유 모듈은 아직 확정하지 못했다.
- **Spec 조항:** `03-spot-actor/05-spot-actor-membership.ko.md` §4의 Join 완료·실패 callback, `06-observability/03-message-flow-tracing.ko.md` §5·6과 `04-flow-correlation.ko.md` §2. Peer 종료 판정은 transport-liveness §5다.
- **교차언어 대조:** Kotlin은 같은 Java Actor Join runtime을 사용한다. Java sample의 `ScenarioSupport.java:118-137`도 이동 시 ZoneChangedNotify를 기다린다. Java 전체 1회와 Kotlin 전체 2회·B7 단독 3회가 통과했으므로 언어 차이나 Kotlin sample 결함으로 단정하지 않는다.
- **변경 분류:** JoinSpot 원인은 A/B/C/D 미판정, 구현 없음. 오류 필드의 formatter 누락은 별도의 B 진단 결함으로 확인했지만 Join 실패의 원인과 동일시하지 않으며 영구 변경하지 않았다.

**수정 전/후 규칙 수:** Join terminal callback 1 → 1, client 이동 완료 조건 1 → 1.
Part 2의 runtime 상태·timer·retry 규칙 추가 0.

### 재현 결과

요청한 full 실행은 `bash samples/kotlin/ZoneWorld/run_sample.sh` ×2와
`bash samples/java/ZoneWorld/run_sample.sh` ×1이다. Full Kotlin 2회에서 재현되지 않아
같은 runner에 `ZW-B7` selector를 지정해 3회 추가 실행했다. 추가 반복은 첫 실패에서
중단하도록 설정했으나 모두 통과했다. 모든 run dir와 로그를 보존했다.

| 실행 | 결과 | main run directory |
|---|---|---|
| 이전 Kotlin 전체 1회 | B7 FAIL, client timeout | `tmp.mr8E3oTuf3` |
| Kotlin 전체 1 | PASS, exit 0, B7·E5 PASS, `zoneworld=completed` | `tmp.a2Wpg4igHu` |
| Kotlin 전체 2 | PASS, exit 0, B7·E5 PASS, `zoneworld=completed` | `tmp.Jam7b4UBjZ` |
| Java 전체 1 | PASS, exit 0, B7·E5 PASS, `zoneworld=completed` | `tmp.dM77pVyUAv` |
| Kotlin B7 단독 1 | PASS, exit 0 | `tmp.0N1OfIiuFA` |
| Kotlin B7 단독 2 | PASS, exit 0 | `tmp.Ng5ulVaufF` |
| Kotlin B7 단독 3 | PASS, exit 0 | `tmp.BhqyA7jmM2` |

Run directory의 공통 앞부분은 `/dev/shm/zlink-tmp-java/`다. 새 실행의 console log는
증거 root의 `{kotlin,java}-zoneworld-N.log`, `kotlin-b7-focused-N.log`이며 exit code는
`samples.status`, `b7-focused.status`에 있다. 각 full 실행의 G4·B8 child run dir도
console log의 `runDir=`에 기록돼 있다.

첫 Kotlin 성공의 돌아오는 Join은 actor `b7-139364`, flow
`01a072f2-01e0-712c-9196-182c4a91951e`이며 zone-node-1 `:25453`의 송신에서
`:25548`의 reply 수신까지 약 35ms다. 두 번째는 actor `b7-c9a7f9`, flow
`01a072f6-2253-7346-96c5-edb5ce634487`이며 `:25209` → `:25235`, 약 26ms다.
대조 발췌는 `kotlin-{1,2}-b7-correlation.txt`에 보존했다. 이 두 full 실행과 Java full
실행의 role 로그에는 `INTERNAL_FAILURE`와 runtime termination 원인 이벤트가 없다.

## 환경과 변경 범위

- Branch: `main`. 기존 Java runtime·M6A·ShutdownSeal·TransportIdentity diff와 untracked TerminalRetentionTest를 유지한다.
- `TMPDIR=/dev/shm/zlink-tmp-java`, `unset ZLINK_LIBRARY_PATH`.
- Gradle은 `flock -w7200 /tmp/zlink-jvm-gate.lock`; sample은 samples lock 이후 JVM lock을 함께 획득한다. `ZLINK_SAMPLE_KEEP_RUN_DIR=1`로 로그를 보존한다.
- 설치된 Maven `zlink-0.17.0.jar`의 `native/linux-x86_64/libzlink.so` SHA-256은 `785b647b3fa1026959009e6cde6b18b470e438d807b6befdb2e96f0462c027a6`이다. 미패키징 Core commit `1899e82f1a`를 사용하거나 package를 재빌드하지 않았다.
- Core·bindings·다른 언어·shared sample·보호된 문서의 기존 변경은 수정하지 않았다.
- 최종 `git diff --check`는 통과했다. 기존 M6A·ShutdownSeal diff가 작업 시작 시 보존본과 같고, 임시 formatter diff가 없음을 확인했다. 문서 원칙 §9의 독립 검토에서 코드·증거 정합성과 표현 관련 finding은 없었다.

## BLOCKERS

**Part 1 없음.** 요청한 반복 검증은 0 fail이며 기존 Java diff를 유지했다. 이번 영구
추가는 `ZLinkJavaRawMeshNode.java`, `ZLinkJavaRawMeshNodeTransportIdentityTest.java`와
이 보고서다.

**Part 2 원인 확정 미달.** 이전 B7 실패와 client timeout의 순서는 확인했지만, 최초
JoinSpot 예외 상세가 기존 로그에서 손실됐고 새 Kotlin 5회에서 재현되지 않았다.
따라서 원인 수정 완료나 환경 flake 판정을 하지 않는다. 다음 판정에는 동일 실패의
예외 상세를 포함한 flow/stack이 필요하다. 새 timeout·retry·fixture 완화로 실패를
숨기는 변경은 하지 않았다. Core 결함의 증거가 없어 C-API sequence를 추정해 작성하지 않았다.

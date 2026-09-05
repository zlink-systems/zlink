# Java D-098: inbound Hello seal·terminal retention·fixture 대기

D-098 item 3·5는 구현했고 focused 회귀 10/10이 통과했다. Item 6의 retry loop와
추가 live-intent 대기는 제거했지만, M6A에서 이미 종료한 intent를 늦은 READY가
재활성화하는 기존 runtime 결함이 드러났다. Kotlin ZoneWorld도 B7에서 실패해 전체 완료 판정은 BLOCKED다.
감독 검토 대상은 아래 변경과 마지막 BLOCKERS의 추가 B 수정이다. Commit하지 않았다.

## 변경 파일과 근거

아래 경로의 공통 앞부분은 `framework/languages/java/zlink-framework-core/`다.

| 파일:line | 변경과 검증 대상 |
|---|---|
| `src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:6350` | `dispatchAdmission`은 Hello이면 기존 `peerAdmissionSealed`를 먼저 조회한다. Seal이면 descriptor decode·intent 귀속·topology/liveness 변경·Admit/Reject 송신 전에 반환한다. Update와 Draining 게시 경로는 유지한다. |
| 같은 파일 `:138`, `:300`, `:7582`, `:7750` | 기존 5분 retention을 nanos로 표현한다. 최초 operation 수락 시 wire deadline의 남은 시간을 한 번 변환해 `retentionDeadlineNanos`에 저장하고, 이후 eviction은 `nanoTime` 차감으로 판정한다. 기본 시간원은 `System.nanoTime`; package-private constructor의 clock 주입은 회귀 테스트용이다. 새 operation의 wire deadline 유효성 검사와 보고용 timestamp는 wall clock을 유지한다. |
| `src/test/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNodeShutdownSealTest.java:33` | 실제 inproc Hello의 sealed/unsealed 응답·topology를 대조한다. 빈 ROUTER probe는 기존 infrastructure-command 판별기로 구분한다. |
| 같은 파일 `:108` | 이미 admit된 peer에 Draining Update를 보내고, seal 이후 revision 99 Hello는 무시하면서 다음 낮은 revision의 Draining Update를 처리하는지 확인한다. |
| `src/test/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNodeTerminalRetentionTest.java:18` | Fake wall clock 하루 전진·이틀 후진과 monotonic clock을 독립 제어한다. Replay·fingerprint fence·새 wire deadline 만료·original deadline + 5분 경계·unfinished operation 보존을 확인하며 nanoTime wrap도 실행한다. |
| `src/test/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNodeM6ATest.java:565`, `:674`, `:776`, `:1489` | 예외를 삼키며 `replacePeerConnection`을 반복 호출하던 `awaitReplacement`를 제거했다. ERROR fixture는 최초 close 요청의 거절을 명시적으로 단언하고, endpoint-only fixture는 기존 거절 단언을 유지한다. 이 fixture들은 CLOSED peer 행을 노출하지 않으므로 기존 `peerIntentIsClosed` 사실을 관찰한 뒤 replacement를 한 번 호출한다. 기존 fence·admission·예외 단언을 유지한다. |
| `src/test/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNodeTransportIdentityTest.java:328` | CLOSED 관찰 뒤 `!hasLivePeerIntent`를 추가로 기다리던 한 줄을 제거했다. 이후 단언과 replacement 호출은 그대로다. |

Seal boolean을 raw node에 복제하는 안과 기존 coordinator predicate를 사용하는 안을
비교해 후자를 택했다. Retention은 wall-clock 점프를 감지·보정하는 안 대신 처음에만
deadline을 변환하고 기존 slot의 만료값 하나를 monotonic으로 유지한다. 새 timer·retry
정책·public API는 추가하지 않았다.

## 네 줄

- **소유 계층:** Framework host `ZLinkMeshDrainCoordinator`가 shutdown seal을 소유하고 raw mesh가 logical admission과 local operation terminal retention을 소유한다. Physical connection·reconnect는 Core 소유다.
- **Spec 조항:** `05-location-relocation/05-host-relocation-flow.ko.md` §14 step 1(D-098 보강), `03-spot-actor/05-spot-actor-membership.ko.md` §2(original deadline 뒤 5분 terminal replay), `03-spot-actor/03-mesh-node.ko.md` §7.1(3)의 종료 확정 후 replacement, D-095·D-098 item 5의 monotonic 경과 시간 규칙.
- **교차언어 대조:** .NET `ZLinkManagedMeshNode.cs:8088`도 Hello 처리 전에 기존 seal을 읽으며 `MeshNodeShutdownSealTests.cs:149,207`은 sealed/unsealed Hello와 admitted peer의 Update를 검증한다. 같은 runtime `:7494,7980,7996`은 `_deadlineClock.Elapsed`에 기반한 deadline + retention을 사용한다. Java는 operation 수락 시 state lane에서 map을 lazy eviction하는 구조이므로 예약 timer를 복제하지 않고 같은 elapsed-time 의미를 slot에 적용한다. Kotlin은 같은 Java runtime을 사용한다.
- **변경 분류:** B — D-098이 승인한 inbound seal 누락과 D-095의 wall-clock retention 잔여 결함 수정. Item 6은 fixture refactor이며 추가로 발견한 terminal intent 재활성화 runtime 수정은 승인 대기다.

## 수정 전/후 규칙 수

- Admission 허용 근거: outbound는 host seal, inbound는 무조건 처리라는 정책 2 → 동일한 host seal 1. Seal 상태 소유자 1 → 1.
- Local elapsed-time 판정: 기존 runtime의 monotonic과 raw terminal retention의 wall clock 2 → monotonic 1. Slot 만료값 1 → 1, retention 5분 → 5분.
- Fixture replacement 재시도 정책 1 → 0; TransportIdentity의 CLOSED 뒤 추가 live-intent 대기 1 → 0. M6A는 기존 종료 사실 관찰과 1회 replacement로 표현한다.
- 추가로 발견한 runtime 결함의 규칙 수는 아직 바뀌지 않았다. Closed intent의 terminal 판정과 READY 재활성화가 충돌하며, 아래 승인 후 수정 대상이다.

## 환경과 검증

- 작업 위치: `framework/languages/java`, branch `main`.
- `TMPDIR=/dev/shm/zlink-tmp-java`, `unset ZLINK_LIBRARY_PATH`, `ZLINK_JAVA_STREAM_TRACE=1`.
- Gradle은 `flock -w7200 /tmp/zlink-jvm-gate.lock`; Java aggregate와 Kotlin ZoneWorld는 sample lock 이후 JVM lock을 함께 사용한다. `ZLINK_SAMPLE_KEEP_RUN_DIR=1`로 run dir를 보존한다.
- `.artifacts/wsl/maven/systems/zlink/zlink/0.17.0/zlink-0.17.0.jar`의 `native/linux-x86_64/libzlink.so` SHA-256은 `785b647b3fa1026959009e6cde6b18b470e438d807b6befdb2e96f0462c027a6`으로 설치된 Core provenance와 일치한다. Package 재빌드 없음.
- 증거 root: `/tmp/zlink-java-d098/`. Test XML의 실패·stream trace를 run별로 보존했다.

| 검증 | 결과 | 증거 |
|---|---|---|
| Seal·retention 최종 focused | 10/10 PASS, skipped 0 (seal 8 + retention 2) | `seal-retention.log`, `seal-retention-xml/` |
| M6A ×10 / TransportIdentity ×10 | M6A 3/10회 PASS, 총 290개 중 9 FAIL; TransportIdentity 10/10회 PASS, 110/110 | `repeats.status`, `repeat-{1..10}.log`, `repeat-{1..10}-xml/` |
| Core test·contract gate ×2 | 두 실행 모두 core 1,264개 중 같은 M6A 2 FAIL; contract 96/96 PASS, skipped 0 | `gates.status`, `gate-{1,2}.log`, `gate-{1,2}-xml/` |
| Java samples ×1 | 7/7 PASS, E5-arm·E5 PASS, exit 0 | `java-samples.log`, `java-samples.exit` |
| Kotlin ZoneWorld ×1 | FAIL: ZW-B7 client TimeoutException, exit 1; E5-arm·E5 PASS | `kotlin-zoneworld.log`, `kotlin-zoneworld.exit` |

Gate 명령은 `./gradlew --no-daemon :zlink-framework-core:test --rerun contractTest --rerun --continue`다.
Gradle의 `--rerun`은 해당 task에 적용되므로 두 task에 명시해 `:test`가 UP-TO-DATE로
생략되는 것을 방지한다. Contract 합계는 각 module의 `contractTest` XML만 포함한다.
두 gate log `:23`에 `> Task :zlink-framework-core:test`가 있고 `:31`에 1,264개 실행이
기록됐다. Core XML의 leaf testcase 합계는 각 1,251개이며 실패는 같은 2건이다.
전체 gate 추가 반복으로 green을 선별하지 않았다.

### Java E5 증거

Java ZoneWorld main run directory는 `/dev/shm/zlink-tmp-java/tmp.aqovoW4BOZ/`다.
`java-samples.log:524-527`에 E5-arm 통과, node-2 재시작(PID 65020), E5 통과가
기록됐다. 재시작 RID는 `zn-b80cbf83-67b9-47cd-b7de-bec082c6bbae`다.
`logs/zone-node-1.log:90754`에 재시작 node-2의 Hello 수락,
`logs/zone-node-2.log:101640`에 node-1의 Admit 수락 완료 진단이 있다.
발췌는 `/tmp/zlink-java-d098/java-e5-admission.txt`에 보존했다.

### Kotlin ZoneWorld 증거

Main run directory는 `/dev/shm/zlink-tmp-java/tmp.mr8E3oTuf3/`다.
`kotlin-zoneworld.log:222-230`과 `logs/client.log:13`에 B7의 client `TimeoutException`이
있다. E5-arm·E5는 `kotlin-zoneworld.log:253-256`에서 통과했다.
B7은 `samples/kotlin/ZoneWorld/Client/src/main/kotlin/systems/zlink/samples/kotlin/zoneworld/client/Scenarios.kt:174`
의 A→B→A 이동·binding 유지 시나리오다.

기존 message-flow를 읽었으며 임시 로깅이나 재실행은 하지 않았다. Actor `b7-0f4733`의
마지막 MoveMsg flow는 `01a072e3-2473-786d-9e4b-31fd6d3395ae`다.
`gateway.log:2784-2791`의 수신·dispatch·actor 송신, `zone-node-1.log:25291-25296`의
actor 수신·dispatch·완료가 모두 성공한 뒤, 같은 flow의 `JoinSpot` request가
`zone-node-1.log:25303`에 송신되고 `:25480-25481`에서 `handler_exception` /
`Failed kind=INTERNAL_FAILURE`로 끝난다. Flow 발췌는
`/tmp/zlink-java-d098/kotlin-b7-flow.txt`에 보존했다. Java의 같은 B7은
`java-samples.log:502`에서 통과했다. 이번 변경과의 인과관계나 최하위 예외 원인은
아직 판정하지 않았으며, 이를 기존 baseline 실패로 단정하지 않는다.

## BLOCKERS

**Item 6 / M6A green 조건 미달.** `focused-2-xml`의
`descriptorFenceReplacesEndpointOnlyIntent`, `focused-3-xml`의 같은 test와
`replacementDoesNotSkipAConnectionBeforeItsReadyEvent`에서 종료 관찰 대기가 실패했다.
`repeat-1-xml`에서는 종료 사실을 관찰한 직후 replacement가 다시
`IllegalStateException`을 던졌다. 단순히 대기 시간이 부족한 것이 아니다.

`m6a-first-failure.trace`는 아래 순서를 보존한다.

```text
READY connection=326 flags=1
peer-intent-disconnect intent=1
DISCONNECTED connection=326
peer-intent-closed intent=1 admittedClosed=false
READY connection=332 flags=1
DISCONNECTED connection=332
Hello repeats; intent never becomes closed again
```

Runtime `ZLinkJavaRawMeshNode.java:6844`가 `closedPeerIntents.add(intentId)`로 종료를
게시한 뒤, `:6793`의 `markPeerIntentsActive`가 늦은 READY에서 같은 set entry를
제거한다. `:6786`의 endpoint/RID 일치만으로 종료한 intent가 다시 활성화된다.
기존 retry loop는 재활성화된 intent에 close 요청을 다시 보내 이를 가렸다.

제안(B): `markPeerIntentsActive`가 기존 closed 사실을 조회해 terminal intent를 다시
활성화하지 않게 한다. 새 상태·timer·monitor generation은 필요 없다. Physical READY
생성 원인을 Core 버그로 단정하지 않는다. Framework가 확정한 intent 종료를 취소하는
경로가 직접 확인됐다. `AGENTS.md` §3의 진단 후 감독 승인 규칙에 따라 추가 runtime
수정 승인을 요청했으며, 승인 전에는 이 경로를 변경하지 않았다.

**Kotlin ZoneWorld 완료 조건 미달.** 위 B7 JoinSpot 실패와 뒤따른 client timeout은
추가 원인 분리가 필요하다. 요청한 1회 실행 결과를 보존했으며 Kotlin 코드·assertion·
timeout을 수정하거나 sample을 재실행하지 않았다.

`git diff --check`는 통과했다.

Core·bindings·다른 언어·보호된 문서는 수정하지 않았다. 동시 작업의 변경은 보존했다.

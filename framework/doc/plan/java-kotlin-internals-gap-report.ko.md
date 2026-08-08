# Java/Kotlin Framework internals gap 구현 결과

## 1. 결론

2026-08-08 기준으로 이 문서가 추적하던 20개 gap과 `JVM-SESSION-001`의 owner-layer 구현 및 회귀
검증을 완료했다. Java와 Kotlin은 같은 JVM Framework runtime을 사용하므로, runtime 불변 조건은
`zlink-framework-core`에서 한 번 구현하고 Kotlin에는 coroutine과 reified type처럼 언어 표현에 필요한
projection만 유지했다. 호출자가 mailbox 회계, JSON 변환, relocation record, completion 경쟁과 실행 lane을
직접 다루는 public helper는 추가하지 않았다.

구현 완료와 전체 승인 완료는 구분한다. owner-layer unit test와 전체 JVM module test, Java/Kotlin API
snapshot, local package clean consumer는 현재 source 기준으로 통과했다. Kotlin Bingo sample aggregate도
수정 전 재현된 `play-b` teardown 교착을 `DefaultSpotContext.enqueueLifecycle()`의 serial-turn carrier
경로에서 수정한 뒤 세 차례 연속 client self-check와 모든 role의 `STOPPED/NONE` 종료를 확인했다. Java
ObservabilityOps `OBS-C2`는 User Spot aggregate에 포함된 actor별 handoff metric을 completion terminal에서
기록하도록 보완한 뒤 실제 Java process에서 통과했고, bindings Java 전체 test도 fresh Core package를
지정해 74개 전부 통과했다. 다만 Kotlin ObservabilityOps의 `OBS-C2`는 현재 Kotlin role host와
ObservabilityOps client가 없고 공유 AutomaticTurnDispatch client가 해당 selector를 지원하지 않으므로
Kotlin process evidence에는 포함하지 않았다. 공통 E2E feature map의 미구현 scenario도 별도 작업으로
남아 있으므로 저장소 전체를 완료·승인으로 판정하지 않는다.
공통 E2E 전체 목록에서 이 작업 이전부터 구현되지 않은 scenario는 이번 결과로 통과했다고 판정하지 않는다.

## 2. 계약과 구현 원칙

공개 계약은 `framework/doc/framework/common/spec/`와 Java/Kotlin exact interface를 기준으로 삼았다.
Internals 문서는 공개 API를 새로 만드는 근거가 아니라 lifecycle, ownership, progress와 terminal-once
불변 조건의 구현 기준으로 사용했다.

설계는 다음 경계를 유지한다.

- mailbox count와 byte reservation은 mailbox가 소유하고 owner 단위로 원자적으로 확정한다.
- session replacement는 새 exact identity를 current로 등록하면 호출자에게 즉시 완료를 반환한다. 이전
  session에는 one-way로 통지하며 callback·outbound drain·close를 기다리거나 실패 시 rollback하지 않는다.
- timeout, cancellation, reply와 shutdown 경쟁은 하나의 completion primitive가 terminal winner를 정한다.
- Deadline과 application 실행 자원은 process가 소유하며 topology마다 executor를 새로 만들지 않는다.
- JSON profile, payload ownership과 STREAM frame 상한은 transport 또는 serializer 내부에서 처리한다.
- diagnostics가 꺼져 있으면 event DTO를 만들기 전에 종료한다.

### 최종 종료 전 Codex Sol 검토 관문

최종 종료 판정 전에는 해당 언어의 모든 Gap·PARTIAL·public contract 항목을 대상으로 `Codex Sol`
review를 수행한다. 요약이나 focused test 통과 여부가 아니라 항목별 exact interface, 정식 spec,
production runtime, owner-layer regression, package/clean-consumer와 process evidence를 서로 대조해
다음 사항을 확인한다.

- 항목이 누락되지 않았는지, 완료로 표시한 구현이 실제 계약과 다른 부분이 없는지 확인한다.
- 누락·오판·부분 구현을 발견하면 해당 항목을 GAP 또는 BLOCKED로 되돌리고 owner-layer 수정과 회귀
  증거를 추가한 뒤 같은 Codex Sol review를 반복한다.
- review 대상, 사용한 Codex Sol 모델/effort, 기준 commit 또는 candidate manifest, 발견 사항, 수정
  commit, 재실행한 gate와 판정을 이 문서에 `file:line` 근거와 함께 기록한다. 단일 test, 문서 존재,
  source compile 또는 과거 결과만으로 항목을 clean 처리하지 않는다.

모든 계약·구현 항목이 위 review에서 누락 없이 구현되었다는 판정을 받은 뒤에만 2차 구조 review를
시작한다. 2차 review도 동일한 `Codex Sol`을 사용하며, 대상은 해당 언어의 Framework runtime
production source와 unit test다. 실행 순서는 먼저 production runtime 리팩터링과 회귀 검증을
완료한 뒤 unit test 리팩터링을 진행하는 것으로 고정한다. 다음 네 범주를 각각 검토하고 결과를 기록한다.

1. **성능 비용** — 불필요한 allocation·copy, payload 변환 왕복, lock/mutex/channel/atomic/queue
   contention, hot path의 중복 작업을 확인한다.
2. **불필요한 코드** — dead code와 도달하지 않는 branch, 사용하지 않는 wrapper·alias·helper·fixture·
   파일·dependency를 확인한다.
3. **POSD/DDD 구조** — deep module·information hiding, pass-through와 temporal decomposition,
   caller complexity, 중복 책임을 POSD 관점에서 확인하고 lifecycle·ownership·state transition·
   commit/deadline·terminal failure invariant의 domain owner가 명확한지 DDD 관점에서 확인한다.
4. **unit test 구조** — runtime 리팩터링으로 보존해야 할 observable behavior와 domain invariant를
   기준으로 test를 다시 읽는다. POSD/DDD 관점에서 동일한 의도·계약·fixture를 반복하는 중복 unit
   test는 하나의 명확한 test 또는 공통 parameterized/fixture test로 통합하고, 의미 없는 복제 test,
   private 구현·호출 순서에만 결합된 test는 회귀 증거를 보존한 뒤 제거한다. 통합·삭제 후에는 해당
   owner-layer regression과 aggregate gate를 다시 실행한다.

2차 review에서 Medium 이상 finding이 하나라도 남으면 clean으로 판정하지 않는다. 해당 runtime/test를
수정하고 필요한 owner-layer regression 및 관련 gate를 다시 실행한 뒤 같은 Codex Sol review를
반복한다. Low finding도 처리하거나 명시적으로 잔여 위험으로 승인 기록해야 한다. 두 단계의 review
결과가 모두 `CLEAN`, Medium 이상 `0`, 미실행 필수 gate `0`으로 기록된 경우에만 이 문서의 전체 작업을
완료로 판정한다.

## 3. Gap별 구현 결과

이 절의 `CLOSED`는 해당 owner-layer source와 회귀 검증이 정식 계약에 맞게 반영되었다는 뜻이다.
원문이 별도로 요구한 package provenance와 실제 process E2E는 4절과 5절에서 독립적으로 판정한다.
따라서 여기의 `CLOSED`만으로 bindings 전체 test나 common E2E 전체 준수를 의미하지 않는다.

### 3.1 Public contract와 package

#### JVM-BUILD-001 — bindings package provenance와 clean consumer

**판정: CLOSED**

bindings Java package에 `StreamSocket.disconnectRid(RoutingId)` 공개 API와 native 연결을 추가하고 local Maven
package를 다시 만들었다. Framework package에는 새 shared JSON module을 포함했다. 격리된 Gradle cache를
사용하는 Java/Kotlin packaged-contract consumer가 고정 version의 bindings와 Framework artifact로
compile하고 실행되는 것을 확인했다.

#### JVM-CONTRACT-001 — RouteMesh option parity

**판정: CLOSED**

금지된 `meshNode(String)` 표면을 제거하고 Java/Kotlin exact interface를 같은 세 method로 맞췄다.
Kotlin 영문·한국어 interface 문서도 같은 계약을 설명한다.

#### JVM-CONTRACT-002 — 제거된 diagnostics public API

**판정: CLOSED**

application observer, raw flow/error DTO와 enum, file·label·native diagnostics option을 public package에서
제거했다. Runtime event는 internal diagnostics package 안에 두고 표준 logger와 monitoring dispatcher가
소비한다. Sample과 E2E도 삭제된 file trace를 만들지 않고 role process의 stdout을 검증한다.

#### JVM-CONTRACT-003 — STREAM session send timeout

**판정: CLOSED**

Java `ZLinkSessionSendCall`과 Kotlin projection에 per-call timeout을 추가했다. Admission은 socket timeout과
per-call timeout 가운데 먼저 만료되는 값을 사용한다. Deadline 이후에는 send나 replay를 시작하지 않고
terminal 결과를 한 번만 확정한다.

#### JVM-CONTRACT-004 — diagnostics level

**판정: CLOSED**

공개 enum을 `OFF`, `ERRORS`, `NORMAL`, `DETAILED`로 맞추고 기본값을 `ERRORS`로 고정했다. Runtime 비교와
Kotlin extension도 이 네 단계만 사용한다.

### 3.2 Wire, serialization과 backpressure

#### JVM-WIRE-001 — RouteMesh message-size 설정과 wire field

**판정: CLOSED**

RouteMesh public option, runtime setter와 service descriptor에서 Framework 고정 message-size field를
제거했다. RouteMesh는 Core transport가 허용한 frame을 별도 Framework 상한으로 다시 거부하지 않는다.

#### JVM-STREAM-SIZE-001 — STREAM oversize 진단과 연결 종료

**판정: CLOSED**

단일 frame과 segmented frame 모두 상한 초과를 발견하면 handler로 전달하지 않고 `EMSGSIZE` 의미의
`ZLinkStreamMessageTooLargeException`으로 기록한 뒤 해당 RID 연결을 종료한다. 이를 위해 bindings의
공개 RID disconnect API를 사용하며 reflection이나 private 접근을 사용하지 않는다.

#### JVM-WIRE-002 — `framework-json-v1` strict profile

**판정: CLOSED**

새 internal module `zlink-framework-json-internal`이 Framework server와 STREAM connector의 mapper를 함께
만든다. Duplicate key와 대소문자가 다른 property를 거부하고, int64 문자열·padded base64를 사용하며,
부동소수점 값을 정수로 강제 변환하지 않는다. Kotlin constructor metadata를 자동으로 등록하고 `xActorId`
같은 lower-camel property 이름을 그대로 유지한다. 두 package가 서로 다른 JSON 규칙을 갖지 않도록
profile을 한 모듈에 숨겼다.

#### JVM-BACKPRESSURE-001 — mailbox byte와 owner별 회계

**판정: CLOSED**

설정한 count·byte budget을 raw service runtime까지 전달한다. Mailbox는 payload뿐 아니라 metadata와 고정
비용을 포함해 owner별 reservation을 확정하고, enqueue 실패·처리 완료·shutdown에서 정확히 한 번
반납한다.

### 3.3 Session, relocation과 location authority

#### JVM-SESSION-001 — session replacement cleanup

**판정: CLOSED**

승인된 계약은 새 exact identity를 current로 등록한 즉시 public bind 완료를 반환한다. 이전 owner의 ACK,
callback과 close를 기다리지 않으며 실패해도 새 binding을 rollback하지 않는다. 이후
`boundSessionReplaced(51)`을 이전 exact session에 one-way로 보내 Java의
`ZLinkSession.onActorBindingReplaced(...)` 또는 Kotlin의
`ZLinkSuspendingSession.onActorBindingReplacedSuspending(...)`을 실행한다. Application은 callback에서 client
안내를 보낼 수 있지만 connection을 직접 닫지 않는다. Callback이 성공 또는 실패로 terminal이 되면 Framework가
non-blocking scheduler에 timer를 예약하고 turn을 즉시 반환한다. Timer는 100 ms 뒤 connection을 닫으며
outbound queue가 먼저 비어도 시간을 줄이지 않고 sleep이나 execution lane 점유로 기다리지 않는다.

`ZLinkSessionActorsRuntime`은 새 binding을 current로 등록한 직후 bind completion을 반환하고 이전 cleanup
terminal을 기다리지 않는다. `boundSessionReplaced(51)`은 one-way으로 전송하며 actor authority source fence와
이전 session owner/binding target fence를 분리해 확인한다. 수신 runtime은 callback 전에 session을 closing으로
바꾸고 새 inbound application dispatch를 거부하지만 callback outbound send는 허용한다. Callback 성공·실패
terminal은 session lane을 점유하지 않고 scheduler timer를 예약하며, timer가 exact retired identity를 다시
확인한 뒤 100 ms 시점에 `SERVER_DRAIN`과 bounded fallback close를 수행한다.
`ZLinkStreamRuntimeIngressTest`는 callback 실패·stall deadline, duplicate와 stale owner/node generation
fence, same-session idempotence 및 callback 전 inbound 거부를 검증하고,
`ZLinkSessionActorBindingContractTest`는 same-session rebind와 다중 actor cleanup을 검증한다. Wire test는
command 51의 round-trip과 malformed fixture를 확인한다.

다음 독립 gate도 모두 통과했다.

- `:zlink-framework-core:test :zlink-framework-kotlin:test`
- Java/Kotlin API snapshot 및 `bound-session-replaced-v1.json` canonical/malformed fixture
- Java/Kotlin isolated package와 clean consumer
- Java와 Kotlin 각각 `JVM-SESSION-001` 세 process rebind: callback client 안내, 다른 session lane 진행,
  100 ms timer bounded close

이번 작업에서는 정식 spec과 common E2E 문서를 수정하지 않았다. 현재 구현과 증거는 이 plan 항목에만 기록한다.

#### JVM-RELOCATION-001 — handoff backlog 상한

**판정: CLOSED**

Pre-commit과 post-commit queue가 같은 count·byte reservation을 사용한다. 상한을 넘긴 message는 queue에
들어가지 않으며 정식 Framework error kind로 완료된다. Reservation은 commit, abort와 replay 모든 경로에서
반납된다.

#### JVM-RELOCATION-002 — Message Follow error kind

**판정: CLOSED**

Expiry, deadline, stale generation, forwarding loop와 bound 초과를 문자열 예외로 남기지 않고 정식 error
kind로 변환한다. Bound session의 route-switch 직후 old-route message도 target으로 전달한다. Original
operation identity와 deadline을 유지하며 late reply는 completion winner를 바꾸지 않는다.

#### JVM-LIVENESS-001 — `SERVING` 게시 순서

**판정: CLOSED**

Peer 연결, handler와 object runtime 준비가 완료된 뒤 descriptor를 `SERVING`으로 게시한다. Startup 실패
경로에서는 service 가능 상태가 먼저 노출되지 않는다.

#### JVM-LOCATION-001 — `UNAVAILABLE`, Store 오류와 bounded page

**판정: CLOSED**

Exact lookup은 row 존재 여부와 current-owner availability를 구분한다. Store read/list 실패는
`UNAVAILABLE` Framework error로 변환하고, list page는 전체 결과를 메모리에 먼저 보관하지 않고 요청한
범위만 유지한다.

#### JVM-LOCATION-002 — Ready Instance owner loss

**판정: CLOSED**

만료된 Ready owner authority를 true Missing과 구분한다. Owner가 유효하지 않으면 cold activation으로
우회하지 않고 `UNAVAILABLE`로 완료하며, authority가 복구된 뒤에만 기존 Ready instance를 다시 사용한다.

### 3.4 Diagnostics, progress, completion과 ownership

#### JVM-DIAGNOSTICS-001 — `OFF` hot-path allocation

**판정: CLOSED**

Dispatch error와 message-flow 경로는 diagnostics level을 먼저 검사한다. `OFF`에서는 event DTO, formatted
message와 observer task를 할당하지 않는다.

#### JVM-PROGRESS-001 — process execution lanes

**판정: CLOSED**

Process가 소유하는 deadline scheduler와 virtual-thread application executor를 추가했다. 각 topology는
공유 executor 위에 순서만 보장하는 serial lane을 만들며 별도 thread pool을 만들지 않는다. 한 topology의
application turn이 대기해도 다른 topology의 작업과 deadline 처리가 진행한다.

#### JVM-COMPLETION-001 — terminal winner

**판정: CLOSED**

Reply, timeout, cancellation과 shutdown 경쟁을 internal completion primitive로 모았다. 각 경로는 독립
boolean을 갱신하지 않고 같은 CAS winner를 사용하며 loser는 payload나 queue reservation만 정리한다.

#### JVM-OWNERSHIP-001 — payload accessor와 retry copy

**판정: CLOSED**

Backend received object는 payload를 필요할 때만 materialize한다. Handoff admission은 공개
`Message.from(Message)` 경계에서 payload를 message-owned frame으로 한 번 복사하고, retry와 replay는
retained packet을 재사용해 byte array 변환을 반복하지 않는다. 외부에는 mutable storage를 노출하지
않는다.

#### JVM-OWNERSHIP-002 — relocation record lazy creation

**판정: CLOSED**

정상 message hot path에서는 relocation record를 만들지 않는다. Relocation sealing이 확정된 뒤에만 frozen
record를 encode하고, retry와 replay는 같은 immutable record를 사용한다.

## 4. 검증 결과

### 4.1 Source와 module test

다음 gate를 통과했다.

```bash
cd framework/languages/java
./gradlew :zlink-framework-core:test :zlink-framework-kotlin:test --no-daemon --max-workers=1
./scripts/verify_api_snapshot.sh java
./scripts/verify_api_snapshot.sh kotlin
./scripts/verify_packaged_contract.sh java
./scripts/verify_packaged_contract.sh kotlin
node ../../runtime/protocol/validate-service-wire-schema.mjs
node ../../runtime/protocol/verify-service-wire-decoder-fixtures.mjs
```

Java API snapshot은 2,788줄과 SHA-256
`f5c94e42e4f4a2fcda2d31eb2d3d39464b6f79d306b9dcefa2dd242b4cb8dde6`이고, Kotlin snapshot은
3,300줄과 SHA-256 `68fcefe30413f52315fa681124070ff02344907f2ddf7950f63e8c51bc80a664`이다.

최신 `:zlink-framework-core:test :zlink-framework-kotlin:test` 실행은 169개 test suite의 931개 test를
`failures=0`, `errors=0`으로 완료했다.

bindings Java의 module export guard는 통과했고 Framework는 bindings의 public contract package만
사용한다. Fresh Core package prefix를 지정한 bindings 전체 test는 74개가 모두 통과했다. 최신
`bindings/java/build/test-results/test/TEST-systems.zlink.contract.CallbackSendContractTest.xml`에는
`tests=5`, `failures=0`, `errors=0`이 기록되어 있다.

주요 owner-layer regression은 다음 동작을 검증한다.

- `ZLinkServiceMailbox`의 count·byte·owner reservation과 release
- `ZLinkSessionActorBindingContractTest`의 즉시 bind completion, one-way 교체 callback, callback terminal
  100 ms 뒤 Framework close, duplicate/stale/pre-restart fence와 다중 actor cleanup 회귀
- `ZLinkActorTransferHandoffTest`의 relocation backlog, error kind와 lazy frozen record
- `ZLinkStoreLocationResolversTest`의 Missing·Unavailable 구분과 Store failure
- `ZLinkMessageFlowTracerTest`와 `ZLinkDispatchErrorReporterTest`의 level과 `OFF` allocation gate
- `ZLinkProcessExecutionLanesTest`와 completion test의 progress·terminal winner
- `ZLinkStreamReceiveBufferTest`, `ZLinkStreamRuntimeIngressTest`의 oversize 차단과 disconnect
- `ZLinkJsonMessageSerializerTest`, `ZLinkStreamJsonTest`와 Kotlin codec boundary test의 동일 strict JSON
  profile, Kotlin constructor와 lower-camel property 처리
- `ZLinkDefaultSpotContextTest.spotWideLifecycleYieldsWhenOnlyTheRestoredSerialTurnIsPresent`의 Kotlin
  coroutine resume에서 serial-turn carrier만 복원된 상태의 lifecycle barrier 진행

### 4.2 Process 증거

다음 process scenario에서 관련 동작 증거를 확보했다.

- InstanceSpot `IS-E2E-05`: 최신 실행에서 Ready owner process를 `SIGKILL`한 뒤 후속 request가
  `UNAVAILABLE`로 bounded completion되고, surviving owner에 새 factory·initialize·handler가 없음을
  확인했다. 최신 log는 `framework/languages/java/e2e/InstanceSpot/logs/20260807-235210-2024824/`이다.
- RegistrationCodec `all`: Java/Kotlin typed JSON 상호 운용
- SpotActorTransfer `ST-F6`: original deadline, reply correlation과 late reply terminal-once
- SpotService `SM-D12`: 실제 server 간 message 처리
- Java AutomaticTurnDispatch `TD-A4`: application turn 대기 중 remote completion progress
- Kotlin AutomaticTurnDispatch `bash run_e2e.sh all`: 실제 Delay/Play/Session 프로세스의
  `ATD-A1..A4`, `ATD-B1..B3`, `ATD-C1..C3`, `ATD-D1..D4`, `ATD-E1..E3` aggregate. `STATUS=0`이며 최신
  증거는 `framework/languages/java/e2e-kotlin/AutomaticTurnDispatch/logs/20260807-205559-2682286/`이다.
  B1/B3은 서로 다른 session의 Actor binding replacement를, C1/C3/D3은 `submit()` 실행 lane의 순서를,
  E3은 독립 recovery session을 통한 shutdown 중 재시작 복구를 실제 프로세스에서 확인한다.
- Java `AutomaticTurnDispatch JVM-SESSION-001`: retired/current 두 connector와 Delay/Play/Session 프로세스를
  실행해 old session의 callback client notice, current session의 `ActorFastReq` 진행, callback terminal 뒤
  `SERVER_DRAIN` 및 100 ms bounded close를 확인했다. 최신 log는
  `framework/languages/java/e2e/AutomaticTurnDispatch/logs/20260808-083911-3624460/`이다.
- Kotlin `AutomaticTurnDispatch JVM-SESSION-001`: Kotlin suspending session callback을 포함한 같은 세
  process sequence를 실행해 동일한 lifecycle 결과를 확인했다. 최신 log는
  `framework/languages/java/e2e-kotlin/AutomaticTurnDispatch/logs/20260808-083935-3631979/`이다.

Kotlin Bingo의 `run_sample.sh all` aggregate는 owner-layer 수정 뒤 세 차례 연속 실행에서 client/server
self-check marker와 7개 role의 `ZLINK_FRAMEWORK_TERMINATION outcome=STOPPED reason=NONE`을 확인했고,
최신 로그는 `framework/languages/java/samples/kotlin/Bingo/build/sample-logs/`에 남아 있다. 그 전에
한 차례 `session-b`가 `FORCE_STOPPED/TEARDOWN_FAILED`로 끝난 실행도 있었으므로, 이 결과를 단일 실행의
우연한 성공으로 해석하지 않고 clean run을 반복해 확인했다. 수정 전 `play-b`에서 발생하던 teardown
교착은 Kotlin coroutine이 thread-local queue owner 없이 serial-turn carrier만 복원하는 경우에도
`yieldCurrent()`가 lifecycle barrier를 현재 turn 앞에서 진행하도록 고쳤다. 또한 monitor callback
executor가 닫히는 경쟁에서 `RejectedExecutionException`이 native callback 밖으로 전파되지 않도록
bindings owner layer를 보완했으며, 최신 Bingo 로그에는 해당 uncaught exception이 없다.

ObservabilityOps `OBS-C2`는 User Spot aggregate completion에서 실제 Actor participant마다
`zlink.drain.actors.handed_off` counter를 terminal 성공 시점에 기록하도록 보완한 뒤 Java process에서
`OBS-C2 PASS`를 확인했다. 최신 log는
`framework/languages/java/e2e/ObservabilityOps/logs/20260808-092026-832564/`이다.
Kotlin runner는 isolated config 전달은 보완했지만 현재 공유 AutomaticTurnDispatch client에 `OBS-C2`
scenario 구현이 없어 `unknown AutomaticTurnDispatch scenario: OBS-C2`로 종료한다. Kotlin tree에는
ObservabilityOps role host와 해당 client가 없으므로 이 process 결과를 Kotlin evidence로 승격하지 않았다.

## 5. 전체 E2E와 최종 승인 경계

이 작업은 공통 E2E 문서의 미구현 scenario를 새 public API나 test-only adapter로 우회하지 않았다.
SpotActorTransfer의 전체 `ST-I5` error-bound matrix와 SubmitAdmission의 미구현 selector처럼 feature map에서
관리하는 scenario는 여전히 별도 작업이다. 이번 구현은 해당 runtime error kind와 bound를 unit test와
`ST-F6` process에서 검증했지만, 미구현 scenario 전체를 통과했다고 뜻하지 않는다. InstanceSpot
`IS-E2E-05`는 위의 실제 crash·lease-loss process evidence를 확보했지만, `IS-E2E-35`는 pending queue를
유지한 뒤 owner loss를 검증하는 별도 process sequence가 없어 아직 미완료다.

최종 승인에서는 기존 gap 20개의 종결 이력, `JVM-SESSION-001`, Framework API/package/module gate,
sample lifecycle gate와 전체 common E2E 준수를 구분한다. `JVM-SESSION-001`은 source, package와 Java/Kotlin
process evidence를 모두 확보해 CLOSED로 판정했다. Java `OBS-C2`와 bindings Java 전체 test도 최신
실행에서 통과했지만, Kotlin ObservabilityOps role host와 각 feature map의 미구현 common E2E scenario가
남아 있어 저장소 전체 준수나 최종 승인은 보류한다. Common E2E 전체 준수는 해당 scenario가 구현되고
실제 process evidence가 확보된 뒤 승인한다.

### 5.1 남은 gate의 정확한 상태

- `bindings/java` 전체 test는 fresh Core package prefix를 지정한 `./gradlew :test --rerun-tasks`에서
  74개 전부 통과했다. `CallbackSendContractTest.xml`도 `failures=0`, `errors=0`이다.
- Java `ObservabilityOps OBS-C2`는 `pending-started`, stdout message-flow, Actor relocation과
  `zlink.drain.actors.handed_off >= 2.0`을 모두 확인해 PASS했다. Kotlin `OBS-C2`는 config 파일 전달
  이후에도 공유 AutomaticTurnDispatch client에 selector가 없어 `unknown AutomaticTurnDispatch scenario`
  로 종료하며, Kotlin ObservabilityOps role host와 client 구현이 선행되어야 한다.
- `InstanceSpot IS-E2E-05`는 Ready owner loss와 bounded `UNAVAILABLE`를 PASS로 기록했다. 그러나
  `IS-E2E-35`는 pending queue를 유지한 owner-crash sequence가 없어 아직 미완료다. `SpotActorTransfer`, `SubmitAdmission`과 나머지 common E2E feature-map 미구현 항목도 같은
  이유로 전체 완료에 포함하지 않는다.

## 6. Codex Sol 독립 최종 리뷰 기록 (완료)

이 절은 최종 종료 전에 수행한 동일 Codex Sol 리뷰의 현재 판정이다. 리뷰어는 동일 세션의 Codex Sol이며,
effort 값은 실행 도구 메타데이터에 노출되지 않았다. 기준 commit은
`137f2858bf7fd29f58405893473be8e773725a93` (`main`, `origin/main`과 동일)이고, 수정은 모두
uncommitted dirty worktree에 있다. 이 리뷰에서 commit·push는 수행하지 않았다.

검토 범위는 이 문서의 JVM gap 20개, Java·Kotlin exact interface와 정식 spec, production runtime,
owner-layer regression, module/API snapshot, package/clean-consumer, protocol fixture와 Java·Kotlin
process evidence다. 대조 순서는 exact interface → 정식 spec → runtime → owner regression → package/
clean-consumer → process evidence로 고정했다.

### 6.1 수정 후 재실행한 finding

- `JVM-BACKPRESSURE-001`은 `ZLinkServiceMailbox.java:59-70,102-134`에서 claimed reservation을
  terminal release까지 count·byte budget에 포함하도록 수정했다. `ZLinkM6ARuntimeContractTest`와
  `ZLinkServiceMailboxCloseTest` 및 전체 module test가 통과했다.
- `JVM-OWNERSHIP-001`은 `ZLinkActorHandoffPacket.java:25-57`에서 caller journal의 입력·accessor
  경계를 clone하고 retained byte 산정을 고정했다. `ZLinkActorTransferHandoffTest`와 전체 module test가
  통과했다.
- `JVM-LIVENESS-001`은 `ZLinkJavaRawMeshNode.java:722-747,761-792`에서 `PREPARING` descriptor를
  유지하고 host startup chain 뒤에만 `SERVING`을 게시하도록 수정했다. `ZLinkFrameworkRuntime.java:252-263`
  의 deferred start와 raw direct-binding 호환 경로를 함께 검증했으며 M6A/M6B owner regression이
  통과했다.
- `JVM-LOCATION-001`은 `ZLinkLocationRuntimeQueryService.java:83-110,124-147,165-233`에서
  provider page를 순차 처리하고 요청 page와 mesh별 summary만 보관하도록 수정했다. page size 상한과
  `LocationStoreContractTest`가 통과했다.

### 6.2 계약 대조 판정과 보호 문서 상태 잔여

- `L-SPEC-STATUS-TABLE` (Low, 비계약 문서 상태 잔여): Java·Kotlin exact interface의 본문과
  production runtime은 command 51, callback, 100 ms timer 계약과 일치하지만, 보호된 정식 문서의
  구현 차이 표가 이전 상태를 기록한다. Java는
  `framework/doc/framework/common/spec/server/languages/java/interfaces/stream-session.ko.md:128-135`,
  Kotlin은 `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/stream-session.ko.md:37-39`,
  공통 차이표는 `framework/doc/framework/common/spec/90-implementation-gap.ko.md:8-19`이다.
  `framework/doc/framework/common/spec/00-public-contract-governance.ko.md:35-42,94-105,173-175`에
  따라 공개 동작의 정본은 공통 spec과 exact interface이고, gap report와 구현 차이 표는 진행 상태를
  기록하는 비권위 감사 자료다. 따라서 이 잔여는 public contract 또는 runtime 불일치가 아니며
  Medium 이상 finding으로 분류하지 않는다. 보호 경로는 사용자 승인 없이 수정하지 않았고, 상태 표를
  정본과 맞추는 변경은 별도 승인 이슈로 남긴다.
- `F-E2E-SF-C5A`는 수정 후 `CLOSED`다. Java와 Kotlin StoreFailure fixture에 실제 exact lookup,
  Missing·Creating·Ready·Unavailable 상태 전이, bounded page parity와 Store failure 전체 page error를
  추가하고 각각 세 process runner에서 검증했다. Java 로그는
  `framework/languages/java/e2e/StoreFailure/logs/20260808-114158-1909255/`, Kotlin 로그는
  `framework/languages/java/e2e-kotlin/DiscoveryRegistryHa/logs/20260808-114226-1950063/`이다. 두 client의
  `scenario-observation SF-C5A exact=unavailable:unavailable,ready:ready,creating:creating` 출력과
  provider/consumer `ZLINK_FRAMEWORK_PEER_READY`, route `message flow` 로그를 확인했다.

### 6.3 재실행 gate와 계약 리뷰 판정

통과한 gate는 `:zlink-framework-core:test :zlink-framework-kotlin:test` (169 suites/928 tests,
failure 0), `:zlink-framework-core:contractTest` (4 suites/27 tests, failure 0), Java·Kotlin API
snapshot, Java·Kotlin packaged clean-consumer, protocol schema/decoder fixture,
`LocationStoreContractTest`, Java/Kotlin JVM-SESSION-001 three-process E2E와 Java·Kotlin SF-C5A
three-process E2E다. JVM-SESSION-001 최신 process 로그는 Java
`framework/languages/java/e2e/AutomaticTurnDispatch/logs/20260808-114056-1865291/`, Kotlin
`framework/languages/java/e2e-kotlin/AutomaticTurnDispatch/logs/20260808-114127-1884769/`이고, SF-C5A
최신 process 로그는 Java `framework/languages/java/e2e/StoreFailure/logs/20260808-114158-1909255/`, Kotlin
`framework/languages/java/e2e-kotlin/DiscoveryRegistryHa/logs/20260808-114226-1950063/`이다. 필수 gate의
미실행/실패 수는 현재 0이다. 이 시점의 계약 대조 결과는 `CLEAN`, Medium 이상 finding 0개이며,
`L-SPEC-STATUS-TABLE`은 보호 문서 수정 없이 기록한 Low 문서 상태 잔여다.

### 6.4 Production Framework runtime POSD/DDD review

계약 review가 `CLEAN`으로 판정된 뒤 동일 Codex Sol로 production runtime을 먼저 검토했다. 검토 범위는
allocation·copy·payload 변환, queue/atomic contention, dead branch와 wrapper, deep module·information
hiding·pass-through, temporal decomposition, caller complexity, lifecycle·ownership·terminal invariant다.

- `JVM-POSD-ONEWAY-001` (Medium, 수정 완료): immutable fluent option을 만든 뒤 `submit()`할 때마다 새
  `AtomicBoolean`이 생겨 같은 logical one-way call이 원본과 변형본에서 두 번 제출될 수 있었다. Java
  bound-session의 direct/native/routed 구현은
  `ZLinkBoundSessionRuntime.java:198-267`, `ZLinkNativeBoundSessionRuntime.java:110-189`,
  `ZLinkRoutedBoundSessionRuntime.java:145-232`에서 하나의 `submitGate`를 변형 객체에 전달하고
  `beginOneWay`로 terminal-once를 보장한다. 같은 결함이 있던 stream reply, channel direct/route/spot와
  Spot direct/deferred/publisher/routed outbound도 동일한 invariant로 정리했다
  (`ZLinkStreamSessionCalls.java:223-263`, `ZLinkChannelDirectCalls.java:166-219`,
  `ZLinkChannelRouteCalls.java:283-496`, `ZLinkChannelSpotCalls.java:89-210`,
  `ZLinkDefaultSpotOutbound.java:201-325`, `ZLinkSpotDirectOutbound.java:333-469,653-757`,
  `ZLinkSpotPublisherRuntime.java:463-588`, `ZLinkSpotRoutedOutbound.java:260-396`).
  `ZLinkBoundSessionSendCallContractTest`와 `ZLinkChannelRuntimeTest.immutableMeshNodeSendOptionsShareTheSingleUseTerminal`
  를 추가해 변형 후 원본 재제출이 `INVALID_OPERATION`으로 종료되고 실제 전송이 한 번인지 검증했다.
  targeted owner regression과 aggregate module test가 모두 통과했다.
- `JVM-POSD-MAILBOX-COPY-001` (Low, 수정 완료): `ZLinkServiceMailbox.java:67-73`에서 `Record` 생성 시
  이미 immutable byte array를 소유하므로 enqueue 때 전체 record를 다시 깊은 복사하던 비용을 제거했다.
  reservation·claim·terminal release invariant는 유지했으며 M6A/mailbox close owner regression과
  aggregate module test가 통과했다.
- 정적 inventory에서 production session-replacement timer 경로의 `Thread.sleep`, coroutine delay,
  `CompletionStage.join/get` 또는 worker 점유를 발견하지 못했다. `ZLinkStreamRuntime`은 callback terminal
  뒤 scheduler timer를 예약하고 session lane을 반환하며, timer가 exact retired identity를 다시 확인한다.
  남은 blocking wait는 startup/monitoring/close와 instance activation의 별도 경로이며
  `ZLinkJavaRawSpotNode.java:2189-2192`의 activation wait도 JVM-SESSION-001의 100 ms timer 경로와 분리되어
  있다. 이 검토에서 Medium 이상 잔여는 없다.

### 6.5 Unit test POSD/DDD review

Production runtime 회귀가 통과한 뒤 Java/Kotlin unit test를 같은 기준으로 다시 읽었다.
`ZLinkSessionActorBindingContractTest`의 replacement completion, callback deadline, stale/fence,
다중 actor cleanup 검증과 `ZLinkStreamRuntimeIngressTest`의 callback-before-close, idempotence/fence,
deadline 검증은 서로 다른 owner invariant를 다루므로 통합하지 않았다. Kotlin callback bridge test도
Java callback과 다른 suspending bridge 계약을 검증한다. 동일 의도·fixture를 반복하거나 private 구현과
호출 순서에만 결합된 test는 발견하지 못해 삭제하지 않았다. 새 terminal-once regression은
`ZLinkBoundSessionSendCallContractTest.java`와 `ZLinkChannelRuntimeTest.java:535-562`에 최소 fixture로
두고, targeted owner regression과 `:zlink-framework-core:test :zlink-framework-kotlin:test`를 다시
실행했다. test 구조 review의 Medium 이상 finding은 0개다.

### 6.6 최종 판정

최종 기준 commit은 계속 `137f2858bf7fd29f58405893473be8e773725a93`이며 변경은 uncommitted dirty worktree에
남겨 두었다. 동일 Codex Sol review의 모델명은 세션 정책상 고정되었고 effort 값은 실행 도구 메타데이터에
노출되지 않았다. 계약 대조와 POSD/DDD review 모두 `CLEAN`이고 Medium 이상 finding은 0개다. 필수 gate의
미실행/실패도 0개다. Java/Kotlin aggregate 169 suites/928 tests, contractTest 4 suites/27 tests,
API snapshot, packaged clean-consumer, protocol canonical/malformed fixture, LocationStore regression,
JVM-SESSION-001 Java/Kotlin three-process evidence와 SF-C5A Java/Kotlin three-process evidence를 현재
runtime 수정 뒤 다시 통과했다. 보호된 formal spec·common E2E 문서는 사용자 지시대로 수정하지 않았고,
오래된 상태 표는 `L-SPEC-STATUS-TABLE` Low 비계약 잔여로 명시했다. 따라서 이 Java/Kotlin Framework 작업의
최종 판정은 `CLEAN / Medium+ 0 / 미실행 필수 gate 0`이다. 별도 common E2E feature-map의 미구현 항목은 이
작업의 계약·runtime gate와 분리된 저장소 전체 과제로 남아 있다.

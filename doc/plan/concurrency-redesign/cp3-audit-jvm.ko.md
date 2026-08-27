# CP3 JVM 감사 — Java monitor·future 경계와 Kotlin 승계

## 1. 결론

**판정: NOT CLEAN.** 테스트를 제외한 JVM Framework 라이브러리 구현을 정적 감사한 결과,
배타적 접근에서 산출한 값·결정이 future/executor/nonblocking transport 경계를 넘는 source는
**97곳**이었다. 이 중 **92곳은 정당화**할 수 있고 **5곳은 결함 의심**이다. 별도로 동기
호환 경계 47곳을 확인했고, 이 가운데 `ZLinkStreamRuntime`의 pending-session `join()` 1곳을
결함 의심으로 판정했다.

- 결함 의심: **[H] 5건, [M] 1건, [C]/[L] 0건**
- Java monitor 취득: **68파일 350곳** (`synchronized` 블록 192, 메서드 158)
- Java `ReentrantLock` 생성 / `*Lock.lock()` 취득: **0 / 0**
- Java 보조 동시성 token: `AtomicX` **378**, `Concurrent*` **156**
- Kotlin monitor·lock 취득: **0**. 보조 token은 `AtomicX` **4**, `Concurrent*` **1**
- `CompletableFuture.complete*` inline dependent 전수 확인: 실제 monitor 안 실행 **11곳**은
  모두 exact terminal을 먼저 확정하므로 제외군이다. 결함 5곳은 이보다 앞 단계인
  “future chain 설치 전 synchronous prefix 실행” 문제다.
- 발견 9의 등록·캡처-before-return 위반은 확인하지 못했다. waiter, claim, operation future는
  모두 외부 callback/반환 전에 설치된다.

사용자가 지정한 branch 이름은 작업 전제값으로만 기록했다. `git` 명령이 전부 금지되어 있어
checkout의 실제 branch 이름은 재확인하지 않았다.

## 2. 범위·기준과 측정 방법

### 2.1 범위

실제 Kotlin 라이브러리 경로는 별도 `framework/languages/kotlin` 트리가 아니라 다음 두 곳이다.

- Java: `framework/languages/java/zlink-*/src/main/java/**/*.java` — **1,014파일**
- Kotlin: `framework/languages/java/zlink-*/src/main/kotlin/**/*.kt` — **18파일**
  (`zlink-framework-kotlin` 17, `zlink-http-client-kotlin` 1)

`samples`, `e2e`, `e2e-kotlin`, `cross-language` 아래의 실행 앱과 모든 test source set은 제외했다.
`zlink-framework-doc-examples`와 `zlink-framework-testkit`의 `src/main/java`는 라이브러리
source set이므로 ① 계수에는 포함했다.

판정 기준은 공통 명세의 금지 형태
(`06-state-ownership-and-lanes.ko.md:54-73`), 작업 프로토콜 예외 조건(`:100-117`),
비재진입·완료 신호·블로킹 경계(`:122-169`), 유형 ①~③(`:171-205`), 반환 전 등록 보존
(`:223-233`), source 계수 단위(`:250-267`), Java `CompletableFuture` inline dependent 규칙이다.
CP3 dotnet 감사와 Node 감사처럼 문자열 hit를 결함 수로 쓰지 않고 실제 source function을
한 번씩 세었다.

### 2.2 취득 계수 재현 명령

아래 명령을 저장소 루트에서 그대로 실행했다. 주석을 제거한 뒤 `synchronized` token 중
뒤에 `(`가 오는 것은 블록, 나머지는 메서드 선언으로 분류한다. 문자열 안 token은 현재
대상에서 hit가 없음을 별도 `rg` 문맥으로 확인했다.

```bash
python3 - <<'PY'
from pathlib import Path
import collections, re
roots = list(Path('framework/languages/java').glob('zlink-*/src/main/java'))
rows = []
for p in sorted(q for root in roots for q in root.rglob('*.java')):
    s = p.read_text()
    s = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), s,
               flags=re.S)
    s = re.sub(r'//[^\n]*', '', s)
    total = len(re.findall(r'\bsynchronized\b', s))
    blocks = len(re.findall(r'\bsynchronized\s*\(', s))
    methods = total - blocks
    reentrant = len(re.findall(r'\bnew\s+ReentrantLock\s*\(', s))
    lock_calls = len(re.findall(r'(?<![\w])\w+(?:Lock|lock)\.lock\s*\(', s))
    if total or reentrant or lock_calls:
        rows.append((p, total, blocks, methods, reentrant, lock_calls))
for row in rows:
    print('|'.join(map(str, row)))
print('TOTAL', *(sum(row[i] for row in rows) for i in range(1, 6)))
PY
```

`AtomicX`와 `Concurrent*`는 취득 지점이 아니라 타입 token 재고다. import를 제외하고 다음
명령으로 파일·모듈별 token을 세었다.

```bash
python3 - <<'PY'
from pathlib import Path
import collections, re
for language, glob in [('java', '*.java'), ('kotlin', '*.kt')]:
    totals = collections.defaultdict(lambda: [0, 0, 0])
    for root in Path('framework/languages/java').glob(
            'zlink-*/src/main/' + language):
        for p in root.rglob(glob):
            s = re.sub(r'/\*.*?\*/', '', p.read_text(), flags=re.S)
            s = re.sub(r'//.*', '', s)
            s = '\n'.join(line for line in s.splitlines()
                          if not re.match(r'\s*import\s+', line))
            atomic = len(re.findall(
                r'\bAtomic(?:Boolean|Integer|Long|Reference|ReferenceArray|'
                r'IntegerArray|LongArray|MarkableReference|StampedReference)\b', s))
            concurrent = len(re.findall(
                r'\bConcurrent(?:HashMap|LinkedQueue|LinkedDeque|SkipListMap|'
                r'SkipListSet|Map|NavigableMap)\b', s))
            totals[root.parents[2].name][0] += atomic
            totals[root.parents[2].name][1] += concurrent
            totals[root.parents[2].name][2] += 1
    print(language, dict(sorted(totals.items())))
PY
```

### 2.3 async source seed와 제외 규칙

seed는 Java method body에 실제 `synchronized` token과 다음 경계 token이 함께 있는 method를
brace-balanced Python scanner로 추출했다.

```bash
rg -n '\bsynchronized\b|CompletableFuture|CompletionStage|\.then(Apply|Compose|Accept|Run)|\.whenComplete|\.handle\(|\.complete(Async|Exceptionally)?\(|\.execute\(|\.submit\(|\.send[A-Za-z]*\(|\.publish[A-Za-z]*\(|\.schedule\(' \
  framework/languages/java/zlink-*/src/main/java -g '*.java'
rg -n '\bsynchronized\b|@Synchronized|ReentrantLock|\.lock\(\)|\bwithLock\b|\bMutex\b|\.await\(|\blaunch\b|withContext' \
  framework/languages/java/zlink-*/src/main/kotlin -g '*.kt'
```

Method/brace scanner의 Java seed는 **93함수**였다. 문자열 검색이 놓친 nested helper의 실제
경계를 수동 호출 추적으로 보충했다: Router `reply`, RawMesh terminal slot, payload
`Assembler.accept` 3곳과 Kotlin lane `run`/`closeAndJoin` 2곳. 이 중
`shutdownFromProcessHook`은 async snapshot이 아니라 blocking bridge이므로 §5로 옮겼다.
따라서 §4의 source는 **97곳**이다.

다음은 source 계수에서 제외했다.

- monitor 안에서 끝나는 getter, option setter, 동기 native 호출
- immutable configuration, 이미 terminal인 future, exact operation identity 자체의 전달
- `ConcurrentHashMap` 단일 연산이나 atomic flag만 있고 복합 상태 snapshot이 없는 곳
- future callback의 sink일 뿐 경계 전 mutable authorization을 새로 읽지 않는 곳
- blocking `join/get`은 중복 계수하지 않고 §5에서 별도 전수 판정

같은 함수 안의 여러 monitor와 여러 continuation은 source 함수 1곳으로 셌다. 다만 서로 다른
overload와 nested class method는 독립 source다. 정당화는 exact identity, serial owner,
lifecycle terminal, 경계 뒤 exact-current 재조회 중 하나를 코드에서 확인한 경우에만 부여했다.

## 3. lock·동기화 취득 전수 계수

분류는 `E` 실행 primitive, `S` socket·dispose 작업 프로토콜, `R` 상태 보호 잔존, `O` 기타다.
한 파일에 여러 책임이 있으면 monitor가 가장 많이 보호하는 주 책임으로 한 번 분류했다.

| 파일 | 합계 | 블록 | 메서드 | 분류 |
|---|---:|---:|---:|---|
| `core/execution/ZLinkAsyncSerialQueue.java` | 33 | 11 | 22 | E |
| `core/execution/ZLinkSpotDispatchQueue.java` | 7 | 7 | 0 | E |
| `core/execution/ZLinkWorkerPool.java` | 2 | 2 | 0 | E |
| `core/runtime/actors/ZLinkActorDispatchSerials.java` | 4 | 4 | 0 | E |
| `core/runtime/actors/ZLinkBoundActor.java` | 1 | 1 | 0 | S |
| `core/runtime/actors/ZLinkDeferredActorJoinScope.java` | 3 | 3 | 0 | S |
| `core/runtime/binding/ZLinkJavaDealerSocket.java` | 13 | 4 | 9 | S |
| `core/runtime/binding/ZLinkJavaRawMeshNode.java` | 6 | 1 | 5 | R |
| `core/runtime/binding/ZLinkJavaRouterSocket.java` | 19 | 0 | 19 | S |
| `core/runtime/binding/ZLinkJavaSocketReceivePoller.java` | 3 | 0 | 3 | S |
| `core/runtime/binding/ZLinkJavaSubscriberSocket.java` | 7 | 0 | 7 | S |
| `core/runtime/channels/RuntimeEndpointConnections.java` | 5 | 0 | 5 | S |
| `core/runtime/channels/ZLinkChannelReceiveLoops.java` | 4 | 4 | 0 | E |
| `core/runtime/channels/ZLinkChannelRouteDispatcher.java` | 1 | 1 | 0 | E |
| `core/runtime/channels/ZLinkChannelSocketRegistry.java` | 2 | 2 | 0 | S |
| `core/runtime/channels/ZLinkSpotRouteBridgeDrainer.java` | 2 | 2 | 0 | E |
| `core/runtime/configuration/ZLinkCodecRegistration.java` | 6 | 2 | 4 | O |
| `core/runtime/host/ZLinkFrameworkRuntime.java` | 3 | 3 | 0 | R |
| `core/runtime/host/ZLinkRelocationShutdownGate.java` | 4 | 0 | 4 | S |
| `core/runtime/host/ZLinkRouteMeshRuntimeView.java` | 3 | 3 | 0 | R |
| `core/runtime/internal/backend/ZLinkBackendActorReceived.java` | 1 | 1 | 0 | S |
| `core/runtime/internal/backend/ZLinkBackendReceived.java` | 1 | 1 | 0 | S |
| `core/runtime/internal/backend/ZLinkBackendStreamReceived.java` | 1 | 1 | 0 | S |
| `core/runtime/internal/channels/ZLinkClientServerRuntimeConfiguration.java` | 1 | 0 | 1 | S |
| `core/runtime/internal/channels/ZLinkFanoutRuntimeConfiguration.java` | 1 | 0 | 1 | S |
| `core/runtime/internal/dispatch/ZLinkApplicationJobContext.java` | 2 | 1 | 1 | E |
| `core/runtime/internal/dispatch/ZLinkApplicationJobQueue.java` | 11 | 11 | 0 | E |
| `core/runtime/internal/dispatch/ZLinkApplicationJobReceiveFlowController.java` | 8 | 8 | 0 | E |
| `core/runtime/internal/drain/ZLinkMeshDrainCoordinator.java` | 5 | 5 | 0 | E |
| `core/runtime/internal/handlers/ZLinkActorHandlerInstances.java` | 3 | 0 | 3 | R |
| `core/runtime/internal/handlers/ZLinkHandlerInstanceOwner.java` | 2 | 1 | 1 | R |
| `core/runtime/internal/monitoring/ZLinkStatusPublisher.java` | 6 | 6 | 0 | E |
| `core/runtime/internal/relocation/ZLinkRetainedSerialQueueCommit.java` | 2 | 2 | 0 | E |
| `core/runtime/internal/service/ZLinkClassicFanoutLiveness.java` | 7 | 0 | 7 | R |
| `core/runtime/internal/service/ZLinkInMemoryLocationAuthority.java` | 4 | 1 | 3 | R |
| `core/runtime/internal/service/ZLinkServiceCompletionDispatcher.java` | 6 | 6 | 0 | E |
| `core/runtime/internal/service/ZLinkServiceMailbox.java` | 6 | 0 | 6 | E |
| `core/runtime/internal/service/ZLinkServiceMailboxScheduler.java` | 8 | 3 | 5 | E |
| `core/runtime/internal/service/ZLinkServiceOperationRegistry.java` | 5 | 5 | 0 | E |
| `core/runtime/locations/ZLinkInMemoryProviderLocationStore.java` | 3 | 3 | 0 | R |
| `core/runtime/locations/ZLinkLocationAutoConnectHost.java` | 2 | 2 | 0 | S |
| `core/runtime/locations/ZLinkStatefulAuthorityRouteRuntime.java` | 2 | 0 | 2 | R |
| `core/runtime/mesh/MeshNodeRegistration.java` | 1 | 0 | 1 | R |
| `core/runtime/spots/ZLinkCanonicalRelocationStateMachine.java` | 14 | 3 | 11 | R |
| `core/runtime/spots/ZLinkDefaultSpotContext.java` | 4 | 3 | 1 | R |
| `core/runtime/spots/ZLinkRelocationPayloadTransfer.java` | 4 | 2 | 2 | S |
| `core/runtime/spots/ZLinkSpotPublisherRuntime.java` | 7 | 1 | 6 | R |
| `core/runtime/spots/ZLinkSpotRetireControl.java` | 3 | 3 | 0 | R |
| `core/runtime/spots/ZLinkStandaloneActorRelocationSourceBuilder.java` | 8 | 3 | 5 | R |
| `core/runtime/spots/ZLinkStandaloneActorRelocationStagingOwner.java` | 7 | 7 | 0 | R |
| `core/runtime/spots/ZLinkUserSpotAggregateStagingOwner.java` | 6 | 6 | 0 | R |
| `core/runtime/spots/ZLinkUserSpotRetireSourceBuilder.java` | 3 | 3 | 0 | R |
| `core/runtime/streams/ZLinkStreamReceiveBuffer.java` | 5 | 0 | 5 | E |
| `core/runtime/streams/ZLinkStreamRuntime.java` | 7 | 2 | 5 | R |
| `locations-redis/ZLinkRedisLocationConnection.java` | 4 | 0 | 4 | S |
| `spring/ZLinkMicrometerMetricSink.java` | 2 | 0 | 2 | O |
| `spring/internal/runtime/ZLinkSpringHandlerFactory.java` | 3 | 1 | 2 | O |
| `spring/internal/runtime/ZLinkFrameworkLifecycle.java` | 6 | 4 | 2 | S |
| `spring/internal/runtime/ZLinkRouteMeshRuntimeService.java` | 12 | 12 | 0 | R |
| `testkit/FakeZLinkBackendAdapterFactory.java` | 1 | 1 | 0 | O |
| `http-client/ZLinkHttpRequestBuilder.java` | 2 | 0 | 2 | S |
| `http-client/internal/CookieJar.java` | 2 | 0 | 2 | R |
| `stream-connector/DefaultZLinkStreamConnector.java` | 1 | 1 | 0 | S |
| `stream-connector/DefaultZLinkStreamSequenceCall.java` | 7 | 7 | 0 | E |
| `stream-connector/ZLinkStreamConnectionLifecycle.java` | 6 | 6 | 0 | S |
| `stream-connector/ZLinkStreamDispatchQueue.java` | 14 | 14 | 0 | E |
| `stream-connector/ZLinkTlsTransportConnection.java` | 3 | 3 | 0 | S |
| `stream-connector/ZLinkWebSocketTransportConnection.java` | 3 | 3 | 0 | S |
| **합계** | **350** | **192** | **158** | **E 138 / S 93 / R 107 / O 12** |

모듈별 보조 token은 다음과 같다. 숫자는 생성 횟수나 contention 횟수가 아니라 import를 뺀
타입 token 출현 수다.

| 언어·모듈 | 파일 | `AtomicX` | `Concurrent*` |
|---|---:|---:|---:|
| Java `zlink-framework-binding-internal` | 67 | 2 | 0 |
| Java `zlink-framework-core` | 757 | 341 | 148 |
| Java `zlink-framework-spring-boot-starter` | 28 | 19 | 3 |
| Java `zlink-framework-testkit` | 4 | 0 | 3 |
| Java `zlink-stream-connector` | 65 | 16 | 2 |
| Java 나머지 8개 모듈 | 93 | 0 | 0 |
| **Java 합계** | **1,014** | **378** | **156** |
| Kotlin `zlink-framework-kotlin` | 17 | 4 | 1 |
| Kotlin `zlink-http-client-kotlin` | 1 | 0 | 0 |
| **Kotlin 합계** | **18** | **4** | **1** |

Kotlin의 5개 token은 `execution/ZLinkStateLane.kt:25-27`의
`ConcurrentLinkedQueue` 1·`AtomicBoolean` 2, `ZLinkOneWayCalls.kt:101`의
`AtomicBoolean` 1, `ZLinkPublisherFlowBridge.kt:19`의 `AtomicReference` 1이 전부다.

진행표 §3의 “683/94클래스 → 후보 32클래스 0”은 L2가 지정한 **전환 후보 집합**에 한정하면
유지된다. 이전 32개 중 C2 전환 대상은 lane/atomic/Concurrent 구조로 옮겨졌고, 그 목록에
남은 monitor는 native adapter와 실행 primitive 제외군이다. 그러나 이를 현재 production 전체의
잔존 monitor가 0이라는 뜻으로 읽을 수는 없다. 현재 실측은 core만 **54파일 284곳**, 전체
라이브러리는 **68파일 350곳**이며, L2 뒤 추가되었거나 당시 상위 후보가 아니었던 R 분류도
107곳이다. 따라서 진행표의 짧은 문구는 후보 집합 지표로는 맞지만 전체 잔존량 설명으로는
불충분하다.

## 4. async·monitor 해제 경계 스냅샷 전수 판정

### 4.1 정당화 92곳

아래 source 수 합계가 92다. 표의 함수 목록은 전 목록이며 같은 함수는 한 행에만 포함했다.

| 분류 | source 수 | 파일:라인·함수 | 정당화 근거 |
|---|---:|---|---|
| 실행·drain primitive | 24 | `execution/ZLinkAsyncSerialQueue.java:184,192,217,255,291,297,310,354,366,387,669,710,728,882,923,1209`; `ZLinkSpotDispatchQueue.java:66,120,134`; `ZLinkWorkerPool.java:100`; `actors/ZLinkActorDispatchSerials.java:141,192,248,293` | queue entry/result/boundary가 exact identity이고 현재 serial owner가 terminal까지 소유한다. completion은 monitor 밖에서 수행한다. |
| native operation·등록 프로토콜 | 8 | `actors/ZLinkBoundActor.java:478`; `ZLinkDeferredActorJoinScope.java:158,302`; `binding/ZLinkJavaDealerSocket.java:37,44`; `ZLinkJavaRouterSocket.java:56,63,75` | disconnect future, Join claim, socket operation을 반환 전에 확정한다. native 제출 뒤 쓰는 것은 mutable route가 아니라 exact operation/소유 socket이다. |
| descriptor·drain·store exact identity | 12 | `binding/ZLinkJavaRawMeshNode.java:574,624,779,7926`; `dispatch/ZLinkApplicationJobQueue.java:128`; `drain/ZLinkMeshDrainCoordinator.java:36,46,93`; `service/ZLinkServiceOperationRegistry.java:46`; `locations/ZLinkInMemoryProviderLocationStore.java:45,60,100` | descriptor revision, waiter, drain-zero future, operation generation, store row 결과가 경계 전 고정된다. 완료 시 같은 identity를 검사하거나 terminal state다. |
| relocation·staging ownership | 23 | `spots/ZLinkDefaultSpotContext.java:872,1053,1097`; `ZLinkRelocationPayloadTransfer.java:146,163,250`; `ZLinkSpotPublisherRuntime.java:487,491`; `ZLinkSpotRetireControl.java:196,248`; `ZLinkStandaloneActorRelocationSourceBuilder.java:973,1081,1095,1117`; `ZLinkStandaloneActorRelocationStagingOwner.java:161,177,350,447`; `ZLinkUserSpotAggregateStagingOwner.java:179,318,345,543`; `ZLinkUserSpotRetireSourceBuilder.java:278` | reservation/slot/fence/backlog을 detach하거나 exact terminal을 먼저 기록한다. replay·discard는 단독 ownership을 넘겨받고, cleanup은 같은 fence를 확인한다. |
| Redis connection exact future | 3 | `locations-redis/ZLinkRedisLocationConnection.java:91 verifySchema`, `:136 connection`, `:156 clearFailedConnection` | `schemaReady`/`connection` future identity가 공유 기준이다. 실패 callback은 `connection == failed`일 때만 지운다. |
| Spring lifecycle·observer terminal | 3 | `spring/.../ZLinkFrameworkLifecycle.java:163 stop`, `:185 stop(Runnable)`; `ZLinkRouteMeshRuntimeService.java:687 MonitorHub.close` | exact runtime를 잡고 stop admission을 닫거나 observer/signal 배열을 detach한 뒤 callback을 호출한다. 새 runtime은 terminal 전 설치되지 않는다. |
| stream connector queue·transport protocol | 17 | `DefaultZLinkStreamSequenceCall.java:73,137,197`; `ZLinkStreamConnectionLifecycle.java:82,296,358`; `ZLinkStreamDispatchQueue.java:57,64,157,211,236`; `ZLinkTlsTransportConnection.java:123,171,183`; `ZLinkWebSocketTransportConnection.java:84,125,137` | waiter/connection-attempt/queued-dispatch를 monitor 안에서 등록·claim·detach한다. predicate/user callback과 waiter completion은 원칙적으로 monitor 밖이며, monitor 안 direct completion은 아래 terminal 검사를 통과한다. |
| Kotlin serial owner | 2 | `execution/ZLinkStateLane.kt:43 run`, `:112 closeAndJoin` | `LaneContext`의 단일 drain owner가 work와 close terminal을 소유한다. `CompletableDeferred` 대기자는 자신의 coroutine context로 재개되어 Java ThreadLocal lane 표시를 상속하지 않는다. |

`CompletableFuture.complete*` inline dependent를 별도 전수 확인했다. 실제 monitor 안에서
완료되는 11곳은 `DefaultZLinkStreamSequenceCall.java:211,233,241`,
`ZLinkWebSocketTransportConnection.java:144`, `ZLinkSpotPublisherRuntime.java:489,493`,
`ZLinkStandaloneActorRelocationSourceBuilder.java:1089`,
`ZLinkMeshDrainCoordinator.java:42,104`, `ZLinkRelocationPayloadTransfer.java:323,327`이다.
각 위치는 success/failure/committed/sealed/assembled 상태와 단독 payload ownership을 먼저
terminal로 만든 뒤 complete한다. 이후 mutable authorization을 쓰지 않으므로 lifecycle
terminal 제외군으로 정당화했다. `ZLinkDeferredActorJoinScope.java:252,255`는 source text상
monitor 안 lambda에 보이지만 lambda가 나중에 mailbox에서 실행되어 실제 monitor 안 완료가
아니므로 11곳에 포함하지 않았다.

### 4.2 결함 의심 5곳

| 심각도 | 위치 | 짧은 근거 | 의심되는 실패 시나리오 |
|---|---|---|---|
| **[H]** | `core/runtime/spots/ZLinkCanonicalRelocationStateMachine.java:502-527 publishReady` | `attempt.ready().thenCompose(...).thenRun(...)`을 monitor 안에서 만들고 L515 뒤에야 `readyPublication`을 설치한다. 이미 완료된 `ready`의 synchronous prefix가 먼저 실행된다. | 같은 thread의 transport callback 또는 매우 빠른 peer 응답이 READY 재시도를 재진입시키면 아직 publication이 null로 보여 같은 fence의 READY/fallback을 두 번 arm할 수 있다. |
| **[H]** | `core/runtime/spots/ZLinkCanonicalRelocationStateMachine.java:927-978 publishTarget` | `prepared().thenCompose(commit...).thenCompose(target::publish)` 전체를 평가한 뒤 L967에 `attempt.publication`을 쓴다. | 이미 완료된 prepare/commit이나 synchronous target publish가 cutover/terminal 경로를 재진입시키면 같은 target fence의 CAS·publish가 중복되고 terminal retention 순서가 역전될 수 있다. |
| **[H]** | `core/runtime/spots/ZLinkSpotRetireControl.java:232-246 publish` | `slot.staged.thenCompose(endpoint.publish)`를 평가한 결과를 그 뒤 `slot.published`에 대입한다. | `staged`가 이미 terminal이고 endpoint가 동기 prefix에서 같은 slot publish를 유발하면 `published == null`을 다시 관찰해 동일 retire publish를 두 번 제출할 수 있다. |
| **[M]** | `locations-redis/ZLinkRedisLocationConnection.java:71-88 closeAsync` | `current.handle(...).thenCompose(closeAsync).thenCompose(client.shutdownAsync)`를 평가한 뒤 `closeStage`를 설치한다. | 이미 완료된 connection stage와 synchronous library completion/reentry가 겹치면 두 번째 close가 `closeStage == null`을 보고 connection/client shutdown을 중복 시작할 수 있다. closed flag가 신규 connection은 막지만 close identity는 아직 없다. |
| **[H]** | `stream-connector/DefaultZLinkStreamConnector.java:277-311 sendFrame` | monitor 안에서 기존 `sendChain.thenCompose(writeFrame)`을 평가한 뒤 새 `sendChain`을 대입한다. | 기존 tail이 완료되어 `writeFrame`이 inline 실행되고 transport callback이 같은 connector에 send를 재진입시키면, 재진입 send가 이전 tail을 다시 사용해 frame write 순서가 갈라지거나 병렬화될 수 있다. |

다섯 건 모두 “future를 변수에 담았으니 직렬화된다”가 아니라 **그 변수에 담기 전 synchronous
prefix**를 문제 삼는다. 후속 수정은 placeholder future를 monitor 안에서 먼저 설치하고 실제
작업을 monitor 밖 scheduler에서 시작하거나, 기존 serial owner에 제출한 뒤 exact identity로
정산하는 방향이어야 한다. 이 감사에서는 수정·재현을 수행하지 않았다.

## 5. 호환 경계(블로킹 브리지)와 반환 전 등록

`.join()` **44곳**, timeout `Future.get(...)` **3곳**, 합계 **47곳**을 전수 확인했다.

| 경계 | 지점 수·위치 | §5 조건 판정 | 잔존 사유 |
|---|---|---|---|
| state-lane 동기 helper | 28 | **충족.** 외부 gate를 잡는 호출은 `ZLinkActorDispatchSerials`의 admission gate 4곳뿐이며 lane work가 그 gate를 다시 잡지 않는다. `ZLinkStateLane.runAsync`는 L58-67에서 `completeAsync`를 사용한다. | 기존 동기 registry/store/activation 표면에서 등록·캡처가 반환 전에 끝나야 한다. 위치: `ActorDispatchSerials:76`, `ActorRuntime:291,4652`, `ActorTransferHandoff:55,509`, `SessionActorsRuntime:102`, `JavaRawServicePort:350`, `JavaRawSpotNode:121`, `JavaStreamSocket:102`, `ChannelSocketRegistry:101`, `ClientServerLocationRuntime:105`, `FanoutLocationRuntime:116`, `ManualFanoutRuntime:76`, `InboundDispatchRegistration:31`, `CompositeRelocationBarrier:37`, `ServiceLivenessRegistry:59`, `ServiceTopologyRegistry:34`, `InMemoryAuthorityStore:1567`, `InMemoryLocationStore:1058`, `LocationRuntime:51`, `ActorJoinPrewarmRegistry:68`, `InstanceSpotActivation:62`, `SpotRelocationReplyRoutes:34,739`, `SpotTimerRegistry:47`, `UserSpotRelocationBarrier:48`, `UserSpotRetireSourceBuilder:1119`, `StreamRuntime:131`. |
| 공유 in-memory store의 same-lane direct path | 위 28 중 2 | **충족.** `InMemoryLocationStore:1054`와 `InMemoryAuthorityStore:1563`의 `isOnLane()` branch는 같은 lane을 공유하는 location→authority private callback만 직접 실행한다. 외부 continuation은 `completeAsync` 뒤 재개되어 이 branch에 도달하지 않는다. | 공유 ownership region 안의 유형 ① private 직접 호출을 보존한다. public 재진입을 허용하는 우회 경로로 사용되는 호출자는 찾지 못했다. |
| 이미 terminal임이 보장된 exact future join | 6 | **충족.** 기다리기 전 `isDone`, `allOf`, 또는 앞선 chain terminal을 확인한다. | sync decode memoization과 terminal materialization: `ZLinkMessage:116`, `ZLinkInboundPayloadOwner:53`, `ZLinkJavaInstanceSpotRegistry:123,140`, `ZLinkCanonicalRelocationStateMachine:984`, `ZLinkStoreLocationResolvers:163`. |
| lifecycle·monitoring·close 브리지 | 12 | **충족.** 대기 전에 Framework monitor/state lane을 보유하지 않고, exact shutdown/settlement/transition stage 하나를 기다린다. 4곳은 명시적 timeout이 있다. | 기존 `Lifecycle`, `AutoCloseable`, 동기 monitoring/callback 계약을 유지한다. `ZLinkFrameworkLifecycle:242`, `ZLinkRouteMeshRuntimeService:244`, `ZLinkRouteMeshRuntimeView:240`, `ZLinkFrameworkRuntime:354,824`, Redis Store `:57/:170`, `ZLinkChannelRuntime:1704`, `ZLinkFanoutLocationRuntime:708`, `ZLinkCompositeRelocationBarrier:51`, `ZLinkJavaStreamSocket:574`, `ZLinkSpotRuntime:5268`. |
| **pending session 생성 대기** | 1 — `ZLinkStreamRuntime.java:1354-1400` | **불충족.** condition ③의 sync 생성 표면 사유는 있으나, creator가 같은 key 생성을 동기 재진입하면 그 creator 자신이 완료해야 할 `claim.pending()`을 L1371에서 기다린다. 완료도 `completeAsync`가 아니라 L1385/L1396의 `completeExceptionally/complete`다. | 기존 단일 생성 결과 공유를 위해 남았지만 비재진입 즉시 실패 조건을 만족하지 않는다. **[H] 결함 의심.** |

마지막 [H]의 실패 시나리오는 user session construction(`createSessionState`)의 동기 prefix가
같은 `(streamNode,routingId)` session lookup/creation을 재호출하는 경우다. 첫 호출은 placeholder의
유일한 producer인데, 둘째 호출이 그 placeholder를 `join()`하므로 첫 호출 스택이 돌아오지 못해
영구 대기한다. spec §5의 “완료 producer와 waiter가 같은 재진입 스택이 되지 않는다”는 조건을
충족하지 못한다.

발견 9의 반환 전 등록은 반대로 지켜진다. 대표적으로 `DeferredActorJoinScope:200-265`는 claim과
intent/barrier를 반환 전에 설치하고, `ServiceOperationRegistry:46-88`은 entry와 timeout을,
`ZLinkStreamDispatchQueue:157-208`은 waiter와 cancellation callback을,
`ZLinkStreamConnectionLifecycle:296-343`은 `connectionAttempt` identity를 외부 starter 호출 전에
설치한다. 등록을 fire-and-forget lane post로 미룬 후보는 찾지 못했다.

Kotlin에는 `.join()`, `runBlocking`, monitor 기반 blocking bridge가 **0곳**이다. Java
`CompletionStage`는 `suspendCancellableCoroutine`/`await`로 비동기 연결된다.

## 6. Kotlin 승계 판정

Kotlin 18파일에는 `synchronized`, `@Synchronized`, `ReentrantLock`, `.lock()`, `withLock`,
`Mutex`가 모두 0이다. 독립적인 mutable aggregate는 `ZLinkStateLane.kt` 한 곳이며,
`ConcurrentLinkedQueue` + 두 `AtomicBoolean` + coroutine drain owner 구조다. same-lane 진입은
`LaneContext`를 확인해 즉시 예외로 끝내고, `run`/`closeAndJoin`은 `CompletableDeferred.await()`로
비동기 대기한다.

`ZLinkOneWayCalls.kt`의 single-use flag와 `ZLinkPublisherFlowBridge.kt`의 subscription reference는
각각 단일 atomic invariant다. 나머지는 Java Framework stage를 coroutine으로 잇는 adapter라
별도 C2 state snapshot을 소유하지 않는다. 따라서 Kotlin 고유 결함 의심은 0이며, Java에서
확인된 6건의 영향은 Java 구현을 호출하는 Kotlin 사용자에게 승계된다.

## 7. POSDDD ② 잔여 후보 상위 10개

POSDDD의 “측정 전 최적화 금지, 불필요한 할당·복사·경합 후보를 측정 대상으로 기록” 원칙에
따라 정적 빈도, payload 크기, 호출 위치를 함께 본 순위다. benchmark, JFR, allocation profile은
실행하지 않았으므로 성능 결함 판정이 아니다.

| 순위 | 위치 | 측정 대상 |
|---:|---|---|
| 1 | `core/runtime/binding/ZLinkJavaRawMeshNode.java:1546-1837,2005-2611,4655-5416,7953-8099` | ingress/relocation frame마다 `ArrayList<byte[]>`와 `clone()`이 겹친다. operation별 copied bytes와 payload lease 해제 시점을 측정한다. |
| 2 | `core/execution/ZLinkAsyncSerialQueue.java:308,400,843-918,1390-1544` | queued record clone, relocation capture `toList/List.copyOf`, retained entry 복사가 hot serial path에 있다. turn당 allocation과 copied bytes를 측정한다. |
| 3 | `core/runtime/spots/ZLinkCanonicalRelocationStateMachine.java:1123-1270,1663-1807` | relocation frame, prepare/cutover byte 배열을 저장·반환할 때 반복 clone한다. fence 1건당 복사량을 측정한다. |
| 4 | `core/runtime/locations/ZLinkInMemoryAuthorityStore.java:199-214,1398,1588` | scan의 전체 sort/materialize와 payload 방어 복사가 authority operation에 결합된다. row 수별 allocation/latency를 측정한다. |
| 5 | `core/runtime/locations/ZLinkInMemoryLocationStore.java:696-764,928` | page마다 stream materialize→sort→새 `ArrayList`→`List.copyOf`가 발생한다. page 크기와 전체 row 수별 이동 원소 수를 측정한다. |
| 6 | `core/runtime/spots/ZLinkRelocationPayloadTransfer.java:59,166,238,274,310-314` | chunk 수만큼 clone한 뒤 완성 payload에 다시 `arraycopy`한다. relocation payload당 peak live bytes와 복사 배수를 측정한다. |
| 7 | `stream-connector/ZLinkStreamDispatchQueue.java:73-77,165-170,215-217,242-244` | waiter/queue 탐색마다 `toList`/`List.copyOf` snapshot을 monitor 안에서 만든다. queue depth별 lock hold time과 allocation을 측정한다. |
| 8 | `spring/.../ZLinkRouteMeshRuntimeService.java:569-699` | poll마다 peer list와 derived event list를 만들고 publish마다 observer/signal 배열을 복제한다. observer 유무별 객체 수와 gate hold time을 측정한다. |
| 9 | `stream-connector/DefaultZLinkStreamSequenceCall.java:39-47,82-105,129,183,241,275-280` | expectation 추가와 각 sequence terminal에서 predicate/message list를 반복 복사한다. sequence 길이별 allocation과 payload 체류 시간을 측정한다. |
| 10 | `core/runtime/internal/monitoring/ZLinkStatusPublisher.java:233-351` | monitor 아래 snapshot과 executor 제출이 status fanout마다 발생한다. observer 수별 contention, queued runnable 수, dropped/coalesced 비율을 측정한다. |

우선 계측값은 operation별 copied bytes, queue/monitor 평균·p99 hold time, allocation count,
relocation peak live bytes다. 계약상 ownership을 위해 필요한 방어 복사와 exact snapshot은 수치 없이
제거하면 안 된다.

## 8. 수행·미수행 범위와 최종 판정

- 수행: 지정 규칙/계획/선행 감사/진행표/POSDDD 문서 읽기, Java 1,014파일·Kotlin 18파일
  static token 스캔, monitor 350곳 파일별 분류, 97개 async source 수동 control-flow 판정,
  blocking wait 47곳과 inline `complete*` 11곳 전수 확인
- 미수행: build, unit/E2E/sample 실행, runtime race 재현, benchmark/JFR/heap profile
- 변경: 이 보고서 1개만 생성. source와 `framework/doc/framework/common/spec/**`는 변경하지 않음
- 제한: branch/dirty state는 `git` 금지 때문에 확인하지 않음. 수치는 현재 읽힌 파일 내용의
  정적 실측이며 runtime 발생 빈도는 추정하지 않음

최종적으로 JVM CP3는 async source의 결함 의심 5건과 blocking bridge의 결함 의심 1건 때문에
**NOT CLEAN**이다. 진행표의 L2 후보 32클래스 전환 완료는 유지되지만, 전체 트리의 monitor
잔존량과 future 설치 순서는 별도 문제다. 후속 작업에서는 5개 future chain에 placeholder-first
설치/monitor 밖 시작을 적용하고, pending session 생성에는 same-producer 재진입을 즉시 실패시키는
fence를 둔 뒤 focused inline-completion·same-key reentry race로 검증해야 한다.

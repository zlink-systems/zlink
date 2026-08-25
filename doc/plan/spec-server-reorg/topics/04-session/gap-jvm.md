# session spec ↔ jvm 구현 대조

검토 기준: f1a2f416f6aa98274af32b9be61e5665dc3c43e7
검토 범위: `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/{streams,actors,binding,internal/service,internal/backend,spots,locations,configuration,channels,host}`,
`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/{streams,errors,locations}`,
`framework/languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/runtime/{streams,actors,binding}`(contract test),
`framework/languages/java/zlink-stream-connector/src`. Kotlin 바인딩(`zlink-framework-kotlin`)은 Java core를 그대로 재사용하며 이번 조사에서 별도 분기는 발견하지 못했다.

## R 규칙 대조

| R# | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| R1 | 일치 | `zlink-framework-core/.../runtime/streams/ZLinkStreamRuntime.java:293-294,791`; `ZLinkBackendStreamSocket.java:29` | `recv()` 모드만 사용, Core packet/raw callback 등록 경로 없음 |
| R2 | 일치 | `ZLinkStreamRuntime.java:906-925`(drainReceiveState) | admission 실패 시 `state.retain(frame)`으로 보류하고 drain 중단, 재전달 없음. contract test: `ZLinkStreamRuntimeIngressTest.java:156-186` |
| R3 | 일치 | `ZLinkStreamSessionContextState.java:215` | `session.onDispatch()` 직접 호출, filter pipeline은 streams 패키지에 미참조 |
| R4 | 일치 | `ZLinkStreamRuntime.java:855-863,1176` | recv routingId가 dispatch까지 보존. contract test: `usesRecvModeAndPreservesTheSourceRoutingId`(`ZLinkStreamRuntimeIngressTest.java:92`) |
| R5 | 일치 | `ZLinkStreamHeader.java:135,151`; `zlink-stream-connector/.../ZLinkTypedStreamRequestCall.java:17` | Response/Error는 name="" 고정, sequence만 사용. `submit(Class<TReply> replyType)`로 client가 타입 지정 |
| R6 | 일치 | `ZLinkStreamRuntime.java:737-761,1491-1524` | closed 플래그로 새 admission 차단, receiveLoops.close() 완료 후에만 stream.close(); closeScheduled CAS로 중복 시작 방지 |
| R7 | 일치(부분 확인) | `runtime/configuration/ZLinkFrameworkRegistration.java:32` | server root(`ZLinkFrameworkRegistration`)당 codec registry 인스턴스 1개는 확인. HTTP client host별·connector instance별 registry의 비공유는 별도 모듈 구조만 확인, 교차 비공유 코드까지 추적 못함 |
| R8 | 일치 | `ZLinkStreamRuntime.java:299-300,1706-1717`; `ZLinkJavaStreamSocket.java:141-145` | onTransportError는 `MonitorEventType.DISCONNECTED`만 구독 → handshake/socket 오류는 session.onError로 안 감. `.onError(` 호출 지점은 코드베이스 전체에서 1곳(`transportErrorDisconnectSessionStage`). handler 예외는 `executeHandler`(1663-1697)가 별도 경로로 처리 |
| R9 | 불일치 | `ZLinkSessionClosingControl.java:9-12`; `ZLinkStreamRuntime.java:1496,1615-1644` | wire 종료 사유 4종(SERVER_DRAIN/IDLE_TIMEOUT/HEARTBEAT_TIMEOUT/PROTOCOL_ERROR)은 정합하나, 계기 쪽은 `server_drain` 문자열을 쓰고(스펙 문구는 `server_shutdown`), `checkSessionLiveness`(1615-1644)는 idle/heartbeat 종료 시 `recordSessionClosed`를 호출하지 않아 `zlink.stream.connections.closed` 지표에 계상되지 않음 |
| R10 | 일치 | `configuration/DefaultZLinkFrameworkOptions.java:167-174`; `streams/StreamBuilders.java:108-111` | 명시 builder 호출만 등록 경로. `@ZLinkStreamPacket`/`@ZLinkStreamRaw` 애노테이션은 이미 등록된 핸들러 내부 메서드 라우팅용일 뿐 |
| R11 | 일치(outbound은 Java 계층 무검증만 확인) | `streams/ZLinkStreamReceiveBuffer.java:78-91`; `binding/ZLinkJavaStreamSocket.java:109-110`; `streams/StreamNodeRegistration.java:166-171`; `ZLinkStreamMessageTooLargeException.java:5`(EMSGSIZE=90); `ZLinkStreamRuntime.java:993-1047` | 6-byte prefix 제외 header+payload 합, inbound만 체크, `value==0?-1:value`, 음수→`ZLinkConfigurationException`, 초과 시 `isolatePeer`→로그 후 `disconnectPeer`(개별 error code 없음, close-reason 채널로만 전달) |
| R12 | 일치 | `streams/StreamNodeRegistration.java:117-126` | 이미 sessionType 설정된 node에 재등록 시 `ZLinkConfigurationException` |
| R13 | 일치(핸드셰이크 자체는 Core 소유) | `streams/StreamNodeRegistration.java:105-114`; `streams/StreamBuilders.java:92-105` | cert/key 필수, requireClientCertificate 기본 false. Session 객체는 `getOrCreateSessionState`가 payload 수신 시에만 생성되므로 핸드셰이크 거부 연결은 session 자체가 생성되지 않음 |
| R14 | 일치(세부 조건별 근거 상이, G6 참고) | `DefaultZLinkFrameworkOptions.java:301-306`(이름 공백)`,169-171,294-299`(이름 중복); `StreamNodeRegistration.java:68-73,143-146`(endpoint 없음); `:121-124`(세션 둘 이상=세션 중복, 동일 코드 경로); `:105-110`(cert/key 공백) | (h) "TLS 미설정+client cert 요구" 조합은 `setTlsServer(cert,key,requireClientCertificate)` 시그니처상 requireClientCertificate가 항상 TLS 설정에 종속돼 있어 그 입력 자체를 만들 수 있는 API 표면이 없음(구조적으로 불가능하므로 공대 위반 없음). (d)와 (e)는 코드상 동일한 단일 검사(G6) |
| R15 | 일치 | `framework/streams/ZLinkSession.java:7-28`; `ZLinkStreamRuntime.java:253` | `ZLinkSession`엔 Spot mutation API 없음, `localActorDispatcher = spots::dispatchLocalSessionActor`로 Actor dispatch 제출까지만 연결 |
| R16 | 불일치 | `internal/service/ZLinkServiceWireCodec.java`(COMMAND 상수, 24/36/38/51); `ZLinkStreamRuntime.java:385-452`(handleSessionRelocationRoute/handleSessionRelocationSeal) | command 24·36·38·51 외에 42·43·44(relocation seal/route)도 SessionState를 매칭해 MeshNode 간 전달함 — "24·36·38·51만 전달"이라는 §8 문장과 실제 코드가 다름(다만 42/43/44 세부는 session-actor-binding 문서가 별도로 다룸) |
| R17 | 일치 | `binding/ZLinkJavaStreamSocket.java:698-713`(unbindBinding이 generation을 실어 전달); `actors/ZLinkBoundActor.java:479-497`(`currentBinding.getAsBoolean()`이 false면 즉시 완료, 재제출 없음) | "tombstone" 용어 자체는 없고 generation 부여 unbind 호출로 구현 |
| R18 | 불일치 | `streams/StreamNodeRegistration.java:141-144`; `configuration/ZLinkFrameworkRegistration.java:246-283` | startup validate가 `meshNodes.isEmpty()`만 확인하고 Location Store 요건도 전역 `objectRoleConfigured`에만 걸려있어, actor dispatch를 켠 StreamNode에 연결된 MeshNode에 Object Client/Server role이 전혀 없어도 startup이 거부되지 않을 수 있음 |
| R19 | 일치 | `binding/ZLinkJavaRawSpotNode.java:2060-2074,2212-2226` | Actor당 binding 하나 유지(교체 시 이전 것 통지·제거), Session은 `ZLinkBoundActor`를 Actor별로 각각 보관 |
| R20 | 일치 | `binding/ZLinkJavaRawSpotNode.java:2048-2090` | bind 시 저장한 `StreamBinding`/`RemoteStreamBinding`을 relay가 재사용, message마다 Store 재조회 없음 |
| R21 | 일치 | `actors/ZLinkSessionActorsRuntime.java:2170-2178` | `StoredBindingRoute`가 actorId/objectGeneration, nodeGeneration/authorityOwnerGeneration/ownerLeaseGeneration, bindingGeneration, lastAcceptedSessionSequence 보관; correlation은 request/reply header로 별도 보존 |
| R22 | 일치 | `spots/ZLinkSpotRuntime.java:3839-3880` | payload가 `admitApplicationJob`/`applicationJobQueue`로 target Actor 실행 문맥에 직접 제출, session queue와 분리 |
| R23 | 판단 불가 | `binding/ZLinkJavaRawMeshNode.java:2876-2911`(`ZLinkTerminalWinner`) | terminal-once 자체는 확인했으나 timeout/cancel 뒤 자동 재전송 부재를 전역적으로 검증하지 못함 |
| R24 | 판단 불가 | `streams/ZLinkStreamRuntime.java:1697-1700` | session 종료 시 `sessionContexts.remove` 등은 확인했으나 late reply가 새 binding에 재사용되지 않음을 직접 증명하는 경로를 특정하지 못함 |
| R25 | 일치 | `binding/ZLinkJavaRawSpotNode.java:728-746` | `destroyActor`가 해당 actorId의 모든 binding(local/remote)을 제거, 이후 재사용되지 않음 |
| R26 | 일치 | `actors/ZLinkSessionActorsRuntime.java:2170-2178` | `StoredBindingRoute` 필드가 스펙 5행 표와 대응 |
| R27 | 일치 | `binding/ZLinkJavaRawMeshNode.java:6250-6300` | command 38 처리에서 ObjectGeneration/NodeGeneration/AuthorityOwnerGeneration 확인 후 단일 reply만 반환 |
| R28 | 일치 | `binding/ZLinkJavaRawSpotNode.java:2305-2325` | `acceptBoundSessionPush`가 bindingGeneration·authorityOwnerGeneration·nodeGeneration 모두 current일 때만 제출(command 36); command 24는 session sequence 파라미터 포함 |
| R29 | 판단 불가 | `actors/ZLinkSessionActorsRuntime.java:513-548`(`bindManagedAsync(ZLinkActor actor)`) | local Actor 객체를 받는 내부 경로가 존재해 "local instance overload 없음" 조항과 상충 여지 — 공개 API 경계까지는 특정 못함 |
| R30 | 판단 불가 | — | 저장 route 무효화 시 Message Follow 단일 전달/`Unavailable` 분기를 코드에서 직접 확인하지 못함 |
| R31 | 판단 불가 | — | owner lease/local admission deadline과 Store 장애 시 미연장을 직접 증명하는 코드를 특정하지 못함 |
| R32 | 일치 | `binding/ZLinkJavaRawSpotNode.java:2210-2216` | `current.sameSessionOwnerEpoch(candidate) && current.bindingGeneration()>=candidate.bindingGeneration()`로 같은 lifecycle 안에서만 비교 |
| R33 | 일치 | `binding/ZLinkJavaRawSpotNode.java:2060-2073` | `installStreamBinding` 성공 후 `notifyBoundSessionReplaced` 결과를 기다리지 않고 즉시 반환 |
| R34 | 일치 | `streams/ZLinkStreamRuntime.java:86-89,527-677` | deadline 5s(`BOUND_SESSION_REPLACEMENT_DEADLINE`), close delay 100ms(`BOUND_SESSION_REPLACEMENT_CLOSE_DELAY`), non-blocking `ScheduledFuture`, closing 상태의 inbound 거부(:1177) |
| R35 | 불일치 | `binding/ZLinkJavaRawServicePort.java:79-122`; `binding/ZLinkJavaRawSpotNode.java:2274,2289` | `sendBoundSessionReplaced`는 단발 `port.send` 호출, 반환된 `CompletionStage`도 버려져 bounded async retry 큐가 실제로 없음(javadoc 약속뿐) |
| R36 | 일치 | `binding/ZLinkJavaRawSpotNode.java:2188-2196` | command 38 tombstone(`!command.active()`) 처리가 정확히 해당 identity만 `remoteStreamBindings.remove` |
| R37 | 일치 | `binding/ZLinkJavaRawSpotNode.java:2067-2072` | `previous.equals(binding)`이면 `notifyBoundSessionReplaced` 미호출(idempotent, 자기 통지 없음) |
| R38 | 일치 | `actors/ZLinkSessionActorsRuntime.java:2188-2199` | `StoredBindingRoute.toTarget`이 actorId/objectGeneration/bindingGeneration 유지한 채 nodeRid/nodeGeneration만 갱신 |
| R39 | 판단 불가 | `binding/ZLinkJavaRawSpotNode.java:2278-2290` | 다른 owner rebind 시 `notifyBoundSessionReplaced`가 one-way로 호출되는 것은 확인했으나 "다른 Actor generation"에 한정된 별도 분기를 명확히 특정 못함 |
| R40 | 불일치 | `binding/ZLinkJavaRawMeshNode.java:6280-6299` | 거부 사유(actor 없음/ObjectGeneration 불일치/route 만료)를 구분하지 않고 동일 실패 코드(102/1)로 응답 — Message Follow relay나 `InvalidOperation`/`Unavailable` 구분 경로를 이 dispatch에서 찾지 못함 |
| R41 | 일치 | `streams/ZLinkStreamRuntime.java:1697-1700` | disconnect 처리(`notifyBoundActorsDisconnectedBestEffort`→`onDisconnected`)가 `destroyActor`나 Spot membership 변경을 호출하지 않음 |
| R42 | 일치 | `actors/ZLinkSessionActorsRuntime.java:351-361`(`notifyDisconnectedAll` — `List.copyOf(bound)` 스냅샷 + `CompletableFuture.allOf`); `actors/ZLinkBoundActor.java:479-486`(`disconnect` 필드로 dedupe, 이미 진행 중이면 같은 future 반환) | all-settled: 개별 `notifyDisconnectedSafely`가 try/catch로 실패 흡수, `whenComplete`에서 나머지 cleanup 계속. `ZLinkSessionActorsRuntime.java` 전체에 Location Store 참조 없음(grep 0건) |
| R43 | 일치 | `spots/ZLinkSpotRuntime.java:3456-3462`(`spot.onDisconnectActor(actor)`); `actors/ZLinkBoundActor.java:475-477`(public `notifyDisconnected()`) | automatic·public 두 통지 모두 동일 `notifyDisconnected(timeout)` 경로를 타며 destroy/membership 변경 없음(R41과 동일 근거) |
| R44 | 일치 | `actors/ZLinkSessionActorsRuntime.java`(applyRelocationSealCommand:929-1037, applyRelocationRouteCommand:1854-1961) | 두 메서드 모두 local fence 검증과 `flight.actor().prepareNativeActorRoute(...)` 호출만 하며 target 선택이나 Location Store I/O가 없음(파일 전체에 LocationStore 참조 0건) |
| R45 | 일치 | `actors/ZLinkSessionActorsRuntime.java:944-991`(seal 설치 시 `sealFenceMatches`, `gate.objectGeneration`, `gate.bindingGeneration`, `gate.seal!=null` 중복 seal 거부); `:1908-1918`(route 적용 시 동일 4값 재확인) | physical Session identity·SessionRid, binding generation+ActorId/ObjectGeneration, relocation identity, seal-binding=route-binding 일치 확인 |
| R46 | 일치 | `internal/service/ZLinkServiceM6BWireCodec.java:1215-1247`(`SessionRelocationSeal`/`SessionRelocationSealed` record) | `SessionRelocationSealed`는 relocation/coordinator/actor/session 필드만 echo, sequence·high-water 필드 없음 |
| R47 | 불일치 | `actors/ZLinkSessionActorsRuntime.java:2031-2083`(`commitPreparedRouteFlight`) | route+snapshot 갱신(`bindingRoutes.put`, :2066) 뒤 **seal 해제**(`gate.seal=null`, :2071)를 먼저 하고 held 목록을 분리(`gate.detachHeld()`, :2072)한 다음 lock 밖에서 `resumeHeld(held)`(:2083) 호출 — spec §8.2가 요구하는 "held 제출 → seal 해제" 순서와 반대로 "seal 해제 → held 제출" 순서로 구현됨(→ G7) |
| R48 | 일치 | `locations/ZLinkLocationOptions.java:16`(기본 3,000ms); `actors/ZLinkSessionActorsRuntime.java:1020-1024`(`armDeadline`); `:1038-1057`(`expireRelocationSeal`→`stream.disconnectPeer`로 physical Session 종료+cleanup); `:1863-1867`(late 시 `late_session_route_update` 로그 후 no-op) | timeout·command44 처리가 `synchronized(sealTerminals)` 같은 직렬 구간에서 `relocationStopped` 플래그로 경합 해소 |
| R49 | 일치 | `actors/ZLinkSessionActorsRuntime.java:1963-2005`(`applyRelocationAbort`) | relay-ready 전 abort: matching seal 해제 후 held를 source route로 재제출(`resumeHeld`), reply 없이 `CompletableFuture<Void>`만 반환(wire상 command 44는 one-way). accepted 이후는 `routeTerminals`/`routeFlights` 캐시가 재개방을 막음(commit 경로에서 동일 key 재수신 시 캐시된 결과만 반환, :1888-1898) |
| R50 | 판단 불가 | `actors/ZLinkSessionActorsRuntime.java`(CAS 이후 source rollback 코드 없음은 확인) | Message Follow가 실제로 target에 전달하는 경로, 서로 다른 connection 간 전역 순서 미보장, session owner process 종료 시 비복원 등은 이 파일 범위에서 직접 확증하지 못함(R53의 shutdown 근거로 일부만 방증) |
| R51 | 일치 | `streams/ZLinkStreamRuntime.java:1445`(session 전용 `ZLinkAsyncSerialQueue`); `actors/ZLinkActorDispatchSerials.java:28-60`(actorId별 독립 큐) | session과 Actor가 서로 다른 큐 인스턴스, 공유 lock/callback stack 없음 |
| R52 | 판단 불가 | `execution/ZLinkAsyncSerialQueue.java:645-707`(lifecycle lane과 application lane 분리) | 구조적 분리는 확인했으나 "비동기 작업 대기 중에도 infrastructure task 진행"을 직접 뒷받침하는 테스트/근거는 찾지 못함 |
| R53 | 일치 | `host/ZLinkFrameworkRuntime.java:1673`(`state.acceptsWork(!drainStarted.get())`로 drain 후 신규 work 거부); `:1337-1346`(deadline 기반 shutdown) | physical connection을 다른 process로 옮기는 코드는 발견되지 않음(부재로 뒷받침) |
| R54 | 일치 | (a) `actors/ZLinkActorClientRuntime.java:443-446`→UNAVAILABLE; (b) `actors/ZLinkActorRuntime.java:2388-2392`→INVALID_OPERATION; (c) `ZLinkActorRuntime.java:2394-2397`→UNAVAILABLE; (d) `spots/ZLinkStandaloneActorRelocationSourceBuilder.java:441-446`→NOT_CONFIGURED; (e) `internal/backend/ZLinkBackendRequestResult.java:85`(actorSessionNotBound=8)→INVALID_OPERATION; (f) `ZLinkBackendRequestResult.java:93-100`(actorLocationStale=21→UNAVAILABLE, spotGenerationStale=33→INVALID_OPERATION); startup 3행: `configuration/ZLinkFrameworkRegistration.java:279-282`, `channels/ChannelRegistration.java:434-451`→NOT_CONFIGURED | 6개 operation 행 + startup 3행 모두 대응 에러 코드 확인 |
| R55 | 판단 불가 | `binding/ZLinkJavaRawMeshNode.java:4344-4358`(제어 record가 application queue를 우회하는 별도 infrastructure-control 경로) | session gate≠Actor gate는 R51 구조로 정황상 부합하나 "두 session callback 동시 실행 안 함" 자체를 검증하는 코드/테스트는 못 찾음 |
| R56 | 불일치 | `binding/ZLinkProcessExecutionLanes.java:13-93` | 직렬 실행 원시 타입은 `ZLinkAsyncSerialQueue` 하나로 통일(부분 일치)되나, lane 정책을 표현하는 sealed/enum 타입(Spot/session/Actor 전달 lane 상태 조합)은 코드베이스에 없음. `ZLinkProcessExecutionLanes`는 Executor 2개를 담는 정적 홀더+단순 FIFO wrapper일 뿐 |
| R57 | 일치 | `streams/ZLinkStreamRuntime.java:1349,1368`(연결마다 `createSessionState`로 새 SessionState, 이전 연결 관계 복원 로직 없음); `actors/ZLinkSessionActorsRuntime.java:913-930`(seal-route 교체로 이동 시 연결 유지) | |
| R58 | 일치 | `actors/ZLinkSessionActorsRuntime.java:964-991`(bindingGeneration/objectGeneration fence 검사, retired binding 거부); `:992-995`(별도 필드 `gate.seal`로 relocation seal 검사) | 서로 다른 필드/전이로 구현되어 두 규칙이 독립적으로 적용됨 |
| R59 | 일치 | `internal/service/ZLinkServiceM6BWireCodec.java:1232-1249`(SessionRelocationSealed에 sequence/high-water 필드 없음); `execution/ZLinkAsyncSerialQueue.java:583-596`(byte-cost admission으로 개별 message 크기 제한은 그대로 적용) | |
| R60-a(Session·Actor gate 분리) | 판단 불가 | `streams/ZLinkStreamRuntime.java`(session 큐), `actors/ZLinkActorDispatchSerials.java`(actor 큐) | 구조적 분리 확인, "두 session callback 동시 실행 안 함"을 직접 단언하는 white-box 테스트는 못 찾음 |
| R60-b(실행 engine 단일성) | 판단 불가 | `execution/ZLinkAsyncSerialQueue.java`(코드 전체에서 다른 직렬 실행 원시 타입 미발견) | 단일성 자체는 정황상 참이나 이를 검증하는 전용 테스트는 못 찾음 |
| R60-c(Lane 정책 조합) | 불일치 | `binding/ZLinkProcessExecutionLanes.java` | R56과 동일 근거 — lane 정책 타입 자체가 없어 "표에 없는 조합을 표현할 수 없다"를 검사할 정적 검사도 존재할 수 없음 |
| R60-d(100 ms 지연 close) | 판단 불가 | `streams/ZLinkStreamRuntime.java:88-89`(`BOUND_SESSION_REPLACEMENT_CLOSE_DELAY = Duration.ofMillis(100)`) | 상수는 존재하나 이를 측정하는 rebind/duplicate-connection 타이밍 테스트를 찾지 못함 |
| R60-e(Seal timeout 기본값) | 일치(정의만) | `locations/ZLinkLocationOptions.java:16`(`Duration.ofMillis(3_000)`) | 기본값 3000ms 정확히 정의됨. 이를 실측하는 테스트는 발견 못함 |
| R60-기타(나머지 26개 항목) | 판단 불가(전수 검증 범위 밖) | — | §14 표 31행 중 위 5행을 제외한 나머지는 이번 조사 범위에서 개별 검증하지 못함 |

## G 항목 — 이 구현의 실제 동작

| G# | 이 구현이 하는 것 | 근거 (파일:줄) |
|---|---|---|
| G1 | rebind 교체 callback의 "lifecycle deadline"은 `Duration.ofSeconds(5)`(`BOUND_SESSION_REPLACEMENT_DEADLINE`), close 지연은 `Duration.ofMillis(100)`(`BOUND_SESSION_REPLACEMENT_CLOSE_DELAY`)로 하드코딩되어 있음 | `streams/ZLinkStreamRuntime.java:86-89` |
| G2 | "Actor factory 없음"은 `NOT_CONFIGURED`(explicit create error)로, "fence stale"은 세부 종류에 따라 `actorLocationStale`(21)→`UNAVAILABLE` 또는 `spotGenerationStale`(33)→`INVALID_OPERATION`으로 구현되어 있음 | `spots/ZLinkStandaloneActorRelocationSourceBuilder.java:441-446`; `internal/backend/ZLinkBackendRequestResult.java:93-100` |
| G3 | command 51(`boundSessionReplaced`) 전송 admission 실패 시 "bounded asynchronous retry"는 실제로 구현되어 있지 않음 — `sendBoundSessionReplaced`는 재시도 없는 단발 `port.send` 한 번뿐이고 반환된 `CompletionStage`도 버려짐(횟수·간격·상한 자체가 코드에 없음) | `binding/ZLinkJavaRawServicePort.java:79-95`; `binding/ZLinkJavaRawMeshNode.java:3167-3178` |
| G4 | 종료 사유는 wire 축(4종: SERVER_DRAIN/IDLE_TIMEOUT/HEARTBEAT_TIMEOUT/PROTOCOL_ERROR, `ZLinkSessionClosingControl`)과 계기(runtime-metrics) 축(4종: client_close/transport_error/protocol_error/server_drain)이 서로 다른 값 집합을 씀 — `server_drain` 문자열이 spec 문구 `server_shutdown`과 다르고, idle_timeout·heartbeat_timeout 종료는 `checkSessionLiveness`가 `recordSessionClosed`를 호출하지 않아 지표에 전혀 계상되지 않음. 실제 소유는 `ZLinkStreamRuntime` 내부에 흩어져 있음 | `streams/ZLinkSessionClosingControl.java:9-12`; `streams/ZLinkStreamRuntime.java:993-1047,1305,1330,1496,1615-1644` |
| G5 | 해당 없음 — `HostPermit`/"host permit" 관련 코드나 문자열이 Java 소스 전체에 없음. 스펙 문서 자체도 §10에서 이 규칙을 execution 주제(46-internal-dispatch-loop)로 이관 중이라고 명시하여, 이 문서 범위에서는 아직 규칙이 미정임 | `session-actor-binding.ko.md:498-500`(스펙); Java 소스 grep 0건 |
| G6 | `registerSession()`은 `sessionType != null` 단일 조건만 검사하므로, "같은 session type 중복 등록"과 "한 node에 session 둘 이상 등록"은 코드상 완전히 동일한 하나의 검사(같은 예외 메시지)이며 별개 검사가 아님. 서로 다른 node에 같은 session 클래스를 등록하는 교차 노드 중복은 어떤 검사에도 걸리지 않음 | `streams/StreamNodeRegistration.java:117-126` |
| G7 | relay-ready 전 abort(command 44 abort) 처리는 "matching seal 해제(`gate.seal=null`) → held message를 source route로 제출(`resumeHeld`)" 순서로 구현되어 있음(seal 먼저, 제출 나중) — spec 20/48이 서술하는 abort 순서와 일치. 다만 동일한 release-then-submit 순서가 commit 경로(`commitPreparedRouteFlight`)에도 그대로 쓰여, spec §8.2가 commit에 요구하는 "held 제출 → seal 해제" 순서와는 반대로 구현되어 있음(→ R47 불일치로 별도 기록) | `actors/ZLinkSessionActorsRuntime.java:1994-2003`(abort); `:2065-2083`(commit) |
| G8 | abort를 보내는 주체를 가리키는 코드 내 이름은 "source coordinator"도 "relocation coordinator"도 아니고, `RelocationRole.SOURCE`/`RelocationRole.TARGET`(역할 enum)와 `RelocationCoordinatorFence`(actor owner 측 authority fence, 별도 개념)로 나뉘어 있음. source 측 구현 클래스명은 "Source" 접미사를 씀(`ZLinkStandaloneActorRelocationSourceBuilder`, `ZLinkUserSpotRetireSourceBuilder`) | `internal/service/ZLinkServiceM6BWireCodec.java`(`RelocationRole`, `RelocationCoordinatorFence` record); `spots/ZLinkStandaloneActorRelocationSourceBuilder.java`; `spots/ZLinkUserSpotRetireSourceBuilder.java`; `actors/ZLinkActorRuntime.java:3184-3203`(`directJoinSessionAbortCommand`가 `RelocationRole.SOURCE` 사용) |

## 요약

- 불일치 8건: R9, R16, R18, R35, R40, R47, R56, R60-c
- 스펙 미정 0건
- 판단 불가 13건: R23, R24, R29, R30, R31, R39, R50, R52, R55, R60-a, R60-b, R60-d, R60-기타

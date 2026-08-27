# 재구성 스펙 ↔ jvm 구현 대조 (2차, 6개 주제)

검토 기준: 22949bcedb67c01e9f4c70d7fd0cd194e817c676
검토 범위: `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/{configuration,errors,monitoring,streams,locations,actors,spots}`,
`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/{configuration,internal/dispatch,internal/service,internal/locations,streams,actors,binding,spots,locations,channels,host,mesh,metrics}`,
`framework/languages/java/zlink-stream-connector/src`, `framework/languages/java/zlink-framework-locations-redis/src`,
`framework/runtime/protocol/generated/jvm/ServiceWireConstants.java`(wire command 상수, 생성 소스).
Kotlin 바인딩(`zlink-framework-kotlin`)은 grep 확인 결과 자체 `ErrorKind`나 옵션 기본값을 정의하지 않고 Java core를 그대로 재사용함(`ZLinkOneWayCalls.kt`만 `ErrorKind` 참조, 값 재정의 없음) — 이번 조사에서 별도 분기는 발견하지 못했다.

이 회차는 "검증 요구 절 전수 대조"가 아니라 **수치·닫힌 값 집합 중심 기계적 스윕**(스펙에서 숫자·백틱 상수·command 번호를 grep으로 추출한 뒤 구현과 대조)과 G9~G21 표적 확인으로 진행했다. 검증 요구 절(`## N. 검증 요구`)은 각 절을 읽었지만 절 안의 모든 bullet을 개별 검증하지는 못했다 — 아래 각 표의 행은 실제로 코드까지 추적해 확인한 항목만 담는다.

도달하지 못한 범위:
- `00-foundation/02-glossary`, `03-overview`, `04-interaction-model`, `05-message-model` — 읽지 않음(용어 정의·개요 문서로 판단, 검증 요구 절 없음)
- `01-execution/02-handler-turn-and-execution-gate`, `03-cancellation-and-shutdown`, `04-spot-timer` — 검증 요구 절만 읽고 개별 bullet은 코드로 추적하지 못함(Yield/Defer 재진입, timer generation/tick 계산, cancellation 경쟁 처리 등)
- `02-channel-transport` 전체 — 검증 요구 절 bullet 대부분과 negative rule을 코드로 추적하지 못함(weight round-robin 순서 `B,A,B,B`, half-open 판정 세부, discovery pair 제외 로직 등). Command 번호와 heartbeat/half-open 상수만 확인
- `03-spot-actor/05~10`(`spot-actor-membership`, `spot-address-messaging`, `stage-wrapper-on-spot`, `routing`, `object-lifecycle`)과 `README` — 전혀 읽지 못함
- `03-spot-actor/01~04`도 검증 요구 절 일부와 숫자 상수만 확인, bullet 전수 대조는 못함
- `05-location-relocation/01,04,05,06`(location-runtime·relocation-flow·host-relocation-flow·failure-failover-policy)의 검증 요구 절 — 문서 본문 수치 sweep만 하고 `## N. 구현 및 contract test 검증 요구` bullet은 개별 확인하지 못함
- `06-observability/01,03,04`(runtime-monitoring·message-flow-tracing·flow-correlation) — 검증 요구 절과 본문을 읽지 않음. `02-runtime-metrics`도 metric 이름 표와 `close_reason` 값만 확인, 나머지 metric의 label 집합·단위는 개별 대조하지 못함
- G18(수신 독점 3축 정확한 값), G19(Actor/Spot 생성 상태 diagram leaf 이름), G20(Redis page 상한), G21(connection projection API)을 제외한 나머지 후보성 항목은 스펙이 값을 정하지 않은 항목이라 판정하지 않음

## 주제별 대조

### 00-foundation

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| `ErrorKind` 13개, 값 0..12, 이름·순서 | 일치 | `zlink-framework-core/.../errors/ZLinkFrameworkErrorKind.java:3-16` | spec `00-foundation/07-framework-error-model.ko.md:26-40`과 이름·번호 완전 일치 |
| Public 오류 표면에 재시도 hint 없음(§9 negative rule) | 일치(부재로 확인) | grep 0건 — `isRetryable`/`retryHint`/`shouldRetry` 계열은 내부 재제출 스케줄러(`actors/ZLinkActorRetryScheduler.java`, `actors/ZLinkActorSubmitFaults.java`)에만 존재하고 public exception/result 타입에는 없음 | 내부 재시도 로직 존재 자체는 규칙 위반이 아님(공개 표면에만 적용되는 규칙) |
| Core/Application job queue profile 기본값 각각 `Balanced` | 일치 | `configuration/ZLinkCoreHwmProfile.java:7`; `configuration/ZLinkApplicationJobQueueProfile.java:7`; `runtime/configuration/ZLinkInboundDispatchRegistration.java:17-18` | 두 profile enum이 독립 타입이며 각각 `BALANCED` 기본값 |
| Pressure 80% pause / 60% resume | 일치 | `runtime/configuration/ZLinkInboundDispatchRegistration.java:21-22`(`applicationJobQueuePauseThresholdPercent = 80`, `...Resume...Percent = 60`); `runtime/internal/dispatch/ZLinkApplicationJobQueue.java:121-124,310-317`(ceil/floor 기반 permit count 비교) | |
| Root Location `SessionRelocationSealTimeout` 기본값 3,000 ms | 일치 | `locations/ZLinkLocationOptions.java:16` | |
| `RelocationPayloadChunkLimit` 기본 256 KiB / `RelocationInFlightPayloadBudget` 기본 16 MiB | 판단 불가 | — | 해당 옵션 상수를 이번 grep 범위에서 특정하지 못함(시간 예산 초과로 미추적) |
| `RouteCacheMaxAge` 기본 15 s / `MessageFollowDuration` 기본 30 s, 둘 다 0이면 비활성, 양수면 5 s 이상 차이 요구 | 일치 | `locations/ZLinkLocationOptions.java:14-15,270-278`; `runtime/configuration/ZLinkFrameworkRegistration.java:55` | `validateRouteLifetimeRelationship`이 `messageFollowDuration - 5s` 초과 시 거부 |
| Activation concurrency 기본값 128(node당) | 일치 | `runtime/mesh/MeshNodeRegistration.java:87,380-386` | |
| ClientServer application listener `MaxMessageSize` 기본값 16,777,216 bytes | 일치 | `runtime/channels/ConfiguredSocketRuntimeOptions.java:7`(`16_777_216L`) | |
| StreamNode Core STREAM inbound 상한 기본 64 KiB | 일치 | `runtime/streams/StreamNodeRegistration.java:158`(`64L * 1024L`) | session 라운드 R11 근거(`ZLinkStreamReceiveBuffer.java`)와 별도 확인 |
| User Spot creation request 최대 1 MiB | 일치 | `runtime/spots/ZLinkSpotRuntime.java:936`(`envelope.length > 1024 * 1024`) | |
| Location operational query page size 1..1000, encoded page 최대 4 MiB | 일치 | `runtime/locations/ZLinkLocationRuntimeQueryService.java:131-132,555-556`(1..1000); `runtime/locations/ZLinkLocationRuntimeQueryService.java:23`(`MAX_OBJECT_PAGE_BYTES = 4 * 1024 * 1024`) | |
| 송신 codec type cache 최대 1,024개, 한도 초과 후 신규 type 미저장(evict 없음) | 일치 | `runtime/internal/configuration/ZLinkCodecRegistration.java:24,338-351`(`MAX_TYPE_CACHE_ENTRIES = 1024`; `if (sendTypeCache.size() < MAX_TYPE_CACHE_ENTRIES)`로만 저장, 초과분은 매번 재평가) | |
| Metadata 전체 UTF-8 encoded 크기 1024 bytes 상한 | 판단 불가 | — | 해당 검증 지점을 이번 grep 범위에서 특정하지 못함 |

### 01-execution

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| Application job queue receive 상한 3축: 건수 64 / byte 1,048,576(1 MiB) / 경과시간 2,000,000 ns(2 ms) | 일치(구현 값 확인, spec은 exact 값 미지정) | `runtime/internal/dispatch/ZLinkReceiveBatchBudget.java:17-19`(`DEFAULT_MAX_MESSAGES = 64`, `DEFAULT_MAX_BYTES = 1_048_576L`, `DEFAULT_MAX_ELAPSED_NANOS = 2_000_000L`) | G18 답변 — spec `05-application-job-queue-and-backpressure.ko.md §10`은 "건수·byte·경과 시간 셋 중 먼저 닿는 것" 존재만 요구하고 값은 정하지 않음. 이 파일의 doc comment가 "첫 record는 항상 허용, 이후는 세 한도가 열려 있을 때만 admission"이라고 §10 요구를 그대로 반영 |
| Actor Join(`Defer()`) 핸들러당 최대 64개, request당 최대 1 MiB, 합계 최대 8 MiB | 일치 | `runtime/actors/ZLinkDeferredActorJoinScope.java:22-24`(`MAX_JOIN_COUNT = 64`, `MAX_REQUEST_BYTES = 1024*1024`, `MAX_TOTAL_REQUEST_BYTES = 8*1024*1024`); 상한 검사는 `:211,216-217` | |
| Actor Join 기본 timeout 5 s | 판단 불가 | — | `ZLinkDeferredActorJoinScope`에서 join 전용 기본 timeout 상수를 특정하지 못함(다른 5 s 상수 다수 존재하나 join 전용인지 확증 못함) |

### 02-channel-transport

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| Wire v1 command 번호 전체(`1..6,16..31,33,34,36..40,42..44,46..53`) | 일치 | `framework/runtime/protocol/generated/jvm/ServiceWireConstants.java:11-50` | 이름·번호 spec `02-channel-transport/06-wire-protocol.ko.md`와 완전 일치(`reply`=20, `actorJoin`=28, `relocationReady`=30, `relocationCutover`=34, `relocationPrepare`=40, `sessionRelocationSeal`=42, `...Sealed`=43, `...Route`=44, `userSpotCreate`=47, `userSpotClose`=48, `relocationState`=52, `relocationFailed`=53) |
| Reserved ID `7..15,32,35,41,45,54..255` 미사용 | 일치(부재로 확인) | `ServiceWireConstants.java:11-50`에 해당 번호 상수 없음 | |
| Channel weight 기본값 100, 범위 0..10000, `-1`/`10001` 거부 | 일치 | `runtime/channels/ConfiguredSocketRuntimeOptions.java:8`(기본 100); `runtime/channels/ZLinkRouteMeshRuntimeOptionsRuntime.java:118`; `runtime/mesh/MeshNodeRegistration.java:353,897,921`; `runtime/channels/ZLinkChannelRuntime.java:255`(모두 `0..10000` 검사) | ClientServer Server weight도 동일 상수 재사용(파일 공유) |
| ClientServer heartbeat probe 5 s 주기, half-open 15 s | 일치 | `runtime/channels/ZLinkChannelSocketRegistry.java:78-81`(`CLIENT_SERVER_PROBE_INTERVAL_NANOS = 5s`, `CLIENT_SERVER_DEADLINE_NANOS = 15s`) | |

### 03-spot-actor

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| Actor 업무 payload가 Spot callback을 거치지 않고 Actor queue에 직접 제출(§8 negative rule 계열, R22와 동일 취지) | 일치 | `runtime/spots/ZLinkSpotRuntime.java:3848,3867,3882-3895`(`admitApplicationJob`→`applicationJobQueue.acquire()`로 target Actor 실행 문맥에 직접 제출) | session 라운드 R22 근거를 HEAD에서 재확인(라인 3839-3880→3848-3895로 소폭 이동) |
| Actor 생성 결과 public 표면의 종결 leaf 이름(G19: `Failed` vs `Aborted`) | 확인(공개 표면=3leaf, 내부 상태=Failed 사용) | `actors/ZLinkActorCreateResult.java:3-16`(sealed interface, `Existing`/`Created`/`Rejected` 3개만 public leaf, exception은 별도 예외적 완료); `runtime/internal/locations/ZLinkCreationTerminalState.java:3-6`(내부 상태 enum은 `CREATED(1)`/`REJECTED(2)`/`FAILED(3)` — 이름이 `Failed`) | G19 답변. `Abort`라는 이름은 별도 개념(`ZLinkSpotRetireControl.ABORT = 3` — recovery cleanup, non-terminal)으로 존재. 즉 JVM은 "세 번째 종결 leaf" 이름으로 `Failed`를 쓰고 `Aborted`는 쓰지 않음 |

### 05-location-relocation

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| Redis Store 조건부 batch: unique key 합계 최대 2,048개, encoded request 최대 4 MiB | 일치 | `zlink-framework-locations-redis/.../ZLinkRedisOpaqueLocationStore.java:75-76`(`MAXIMUM_BATCH_KEYS = 2048`, `MAXIMUM_ENCODED_BATCH_BYTES = 4*1024*1024`); 검사 `:648` | |
| Store Value 최대 1 MiB | 일치 | `ZLinkRedisOpaqueLocationStore.java:74`(`MAXIMUM_VALUE_BYTES = 1024*1024`) | |
| Store Key 1..1024 bytes | 일치 | `ZLinkRedisOpaqueLocationStore.java:72,790`(`MAXIMUM_KEY_BYTES = 1024`; `"Location Store keys must contain 1..1024 UTF-8 bytes."`) | |
| Scan page limit 1..1000, encoded page 최대 4 MiB | 일치 | `ZLinkRedisOpaqueLocationStore.java:721-723`(`request.limit() < 1 \|\| > 1000`) | 05-location-relocation `02-location-store-redis.ko.md:132`와 일치 |
| `StoreVersion`/cursor 1..4096 bytes | 일치(대상 개념 일치 여부는 판단 보류) | `ZLinkRedisOpaqueLocationStore.java:73,802`(`MAXIMUM_VERSION_BYTES = 4096`; `"Store versions must contain 1..4096 UTF-8 bytes."`) | spec은 "Cursor는 4,096 bytes까지"(`02-location-store-redis.ko.md:286`)라고 쓰는데 코드 상수명은 `StoreVersion`(`version` 필드) 대상 — cursor와 version이 spec에서 같은 개념인지 이번 조사로는 확정하지 못함 |
| G20: 이동 대상 목록 페이지 상한 1,024개 vs Redis batch 2,048-key 상한 | 확인(서로 다른 개념, 모순 아님) | `02-location-store-redis.ko.md:111`(batch 2,048-key)와 `01-location-runtime.ko.md:78`(페이지 1,024개)는 각각 "atomic commit batch"와 "relocation target 목록 page"로 스펙 본문에서도 별도 항목 — JVM 코드에서 이 두 상한에 대응하는 두 개의 서로 다른 상수(`MAXIMUM_BATCH_KEYS=2048`은 확인, "이동 대상 목록 페이지=1024" 전용 상수는 이번 grep 범위에서 특정하지 못함) | 페이지 1,024개 전용 상수를 찾지 못해 완전한 일치 확인은 유보 — 판단 불가에 가까움 |
| G21: connection projection API 존재 여부 | 확인(부재) | 6개 주제 스펙 전체 grep 0건("connection projection" 문구 없음); JVM public configuration 표면(`configuration/ZLinkEndpointConnections.java`, `ZLinkMeshPeerConnections.java`, `runtime/channels/RuntimeEndpointConnections.java`)은 수동 peer 등록 builder일 뿐 live connection을 열거하는 조회 API가 아님 | 이 gap 항목이 참조한 "옛 45번 문서 §4"가 재구성 스펙에서 해당 표현 자체를 쓰지 않게 됨 — 재구성 스펙 6개 주제 안에서는 이 항목이 더 이상 서술되지 않는 것으로 보임(구 문서 자체를 이번 조사 범위 밖이라 직접 대조는 못함) |

### 06-observability

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| `zlink.stream.connections.*` metric 이름(active/opened/closed) | 일치 | `runtime/streams/ZLinkStreamRuntime.java:1355-1365` | 이름 자체는 spec `02-runtime-metrics.ko.md:179-181`과 일치 |
| `close_reason` 닫힌 값 집합 `client_close\|idle_timeout\|heartbeat_timeout\|server_shutdown\|protocol_error\|transport_error` | 불일치 | spec: `02-runtime-metrics.ko.md:192`; 구현: `ZLinkStreamRuntime.java:1305`(`"client_close"`), `:1030`(`"protocol_error"`), `:1330`(`"transport_error"`/`"protocol_error"`), `:1496`(`"server_drain"` — spec의 `server_shutdown`과 다른 문자열), `:1615-1644`(`checkSessionLiveness`가 idle/heartbeat 종료 시 `recordSessionClosed`를 **호출하지 않음** — `idle_timeout`·`heartbeat_timeout` 값은 이 metric에 전혀 기록되지 않음) | session 라운드 R9/G4와 동일 결함이 HEAD(`22949bcedb`)에서도 그대로 존재함을 재확인. 6개 값 중 `server_shutdown`은 다른 문자열로, `idle_timeout`·`heartbeat_timeout` 2개는 아예 미기록 |

## G 항목 — 이 구현의 실제 동작

| G# | 이 구현이 하는 것 | 근거 (파일:줄) |
|---|---|---|
| G9 | (session 주제 근거 재확인) `ZLinkSpotRuntime`은 Actor payload를 `admitApplicationJob`으로 target Actor의 application job queue에 직접 제출하며 Spot global queue로 직렬화하지 않는다 | `runtime/spots/ZLinkSpotRuntime.java:3848,3867,3882-3895` |
| G10 | commit 경로(`commitPreparedRouteFlight`)는 route/snapshot 갱신 → seal 해제(`gate.seal=null`) → held 분리(`gate.detachHeld()`) → lock 밖에서 `resumeHeld(held)` 순서로, "해제 후 제출"이다 | `runtime/actors/ZLinkSessionActorsRuntime.java:2031-2083`(seal=null:2071, detachHeld:2072, resumeHeld:2083) |
| G11 | late/duplicate command 44는 `late_session_route_update`류 로그만 남기고 no-op 처리한다(session 라운드 R48 근거, HEAD에서 구조 동일) | `runtime/actors/ZLinkSessionActorsRuntime.java` 내 late 판정 분기(:1863-1867 부근, session 라운드 인용과 동일 위치대) |
| G12 | admission 실패 시 새 packet을 읽지 않는 admission 중단은 연결별이 아니라 **node(StreamRuntime) 전체** drain 중단이다 | `runtime/streams/ZLinkStreamRuntime.java`(`drainReceiveState`, session 라운드 R2 근거와 동일 구조) |
| G13 | bind 시 target에 Actor가 없고 Message Follow route가 있는 경우의 relay는 사유별로 구분되지 않고 동일 실패 코드로 응답한다(session 라운드 R40 근거 재확인, HEAD에서 심볼 위치 유지) | `runtime/binding/ZLinkJavaRawMeshNode.java`(command 38 처리, 거부 사유 미구분) |
| G14 | envelope 보존 값은 `bindingGeneration`(generation)이며 "binding token"이라는 별도 타입/필드는 wire schema에 없다 | `runtime/actors/ZLinkSessionActorsRuntime.java`(`StoredBindingRoute` 필드 — actorId/objectGeneration/bindingGeneration 등) |
| G15 | lane 정책을 표현하는 sealed/enum 타입이 코드베이스에 없고, 직렬 실행 원시 타입은 `ZLinkAsyncSerialQueue` 하나로 통일되어 있다 | `runtime/binding/ZLinkProcessExecutionLanes.java`(Executor 2개를 담는 정적 홀더) |
| G16 | 단일 언어 이탈 묶음 중 JVM 몫은 R18(startup validate가 MeshNode당 Object role 요건을 전역 `objectRoleConfigured`로만 검사)과 R35(`sendBoundSessionReplaced`가 재시도 없는 단발 `port.send`)다 | `runtime/streams/StreamNodeRegistration.java:141-144`; `runtime/configuration/ZLinkFrameworkRegistration.java:246-283`; `runtime/binding/ZLinkJavaRawServicePort.java:79-122` |
| G17 | node 경계를 넘는 record는 wire command 24·36·38·51(application record)뿐 아니라 relocation control 42·43·44도 Session owner와 coordinator 사이를 넘어 전달된다 | `internal/service/ZLinkServiceWireCodec.java`(COMMAND 24/36/38/51 나열); `runtime/streams/ZLinkStreamRuntime.java`(`handleSessionRelocationRoute`/`handleSessionRelocationSeal`이 42/43/44를 별도로 처리) |
| G18 | Application job queue 수신 회전의 3축 상한은 건수 64, byte 1,048,576(1 MiB), 경과시간 2,000,000 ns(2 ms)로 하드코딩되어 있다 | `runtime/internal/dispatch/ZLinkReceiveBatchBudget.java:17-19` |
| G19 | Actor/User Spot 생성 public 결과는 3-leaf sealed 타입(`Existing`/`Created`/`Rejected`)이고, 내부 상태 enum에서 세 번째 종결 leaf는 이름이 `FAILED`다(`Aborted`라는 이름은 쓰지 않는다). `Abort`는 recovery cleanup을 가리키는 별개의 non-terminal 개념 이름이다 | `actors/ZLinkActorCreateResult.java:3-16`; `runtime/internal/locations/ZLinkCreationTerminalState.java:3-6`; `runtime/spots/ZLinkSpotRetireControl.java:40`(`ABORT = 3`) |
| G20 | Redis atomic batch commit은 최대 2,048 unique key/4 MiB로 구현되어 있다(스펙 `02-location-store-redis.ko.md`와 일치). "이동 대상 목록 페이지 1,024개" 전용 상수는 이번 조사에서 별도로 찾지 못했다 | `zlink-framework-locations-redis/.../ZLinkRedisOpaqueLocationStore.java:75-76,648` |
| G21 | "connection projection" API는 재구성 스펙 6개 주제 전체와 JVM public configuration 표면 어디에도 없다 — 존재하는 것은 수동 peer 등록 builder(`ZLinkEndpointConnections`, `ZLinkMeshPeerConnections`)뿐, live connection을 열거하는 조회 API가 아니다 | grep 0건(6개 주제 spec); `configuration/ZLinkEndpointConnections.java`; `configuration/ZLinkMeshPeerConnections.java` |

## 요약

- 불일치 2건: 06-observability `close_reason` 값 집합(`server_drain`≠`server_shutdown`, idle/heartbeat 미기록) — session 라운드 R9/G4가 HEAD에서도 유효함을 재확인. G16(R18·R35, session 라운드에서 이미 불일치로 기록된 항목의 재확인)
- 스펙 미정 1건: G18(수신 상한 3축 exact 값 — 스펙이 정하지 않음, JVM 구현 값만 기록)
- 판단 불가 5건: 01-execution의 Actor Join 기본 timeout 5s 근거 미특정, 00-foundation의 `RelocationPayloadChunkLimit`/`RelocationInFlightPayloadBudget`/metadata 1024 bytes 근거 미특정, 05-location-relocation의 "이동 대상 목록 페이지 1,024개" 전용 상수 미특정(G20 부분), StoreVersion과 spec의 "cursor" 개념 동일성 미확정
- 이번 회차는 수치·닫힌 값 집합과 G9~G21에 예산을 집중했고, 위 "도달하지 못한 범위"에 나열한 검증 요구 bullet 대다수(특히 02-channel-transport 전체, 03-spot-actor 05~10, 05-location-relocation의 검증 요구 절, 06-observability 01/03/04)는 이번 조사에서 다루지 못했다

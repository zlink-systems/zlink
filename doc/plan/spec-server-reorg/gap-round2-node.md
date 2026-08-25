# 재구성 스펙 ↔ node 구현 대조 (2차, 6개 주제)

검토 기준: `22949bcedb67c01e9f4c70d7fd0cd194e817c676`
검토 범위: `framework/languages/node/packages/framework/src/{contracts,runtime}` 전체(특히
`runtime/actors`, `runtime/channels`, `runtime/host`, `runtime/locations`, `runtime/spots`,
`runtime/streams`, `runtime/codecs`, `runtime/foundation`), `framework/languages/node/test/contract`
디렉터리 존재 확인. 6개 주제(00-foundation, 01-execution, 02-channel-transport, 03-spot-actor,
05-location-relocation, 06-observability) 전체 45개 문서의 `## N. 검증 요구` 절을 모두 추출해
읽었다.

도달하지 못한 범위: 대상 범위가 매우 넓어(검증 요구 절만 총 1,400+줄) 우선순위 1~3(수치·닫힌
값 집합·부정 규칙)과 G18~G21에 예산을 집중했다. 다음은 근거를 확보하지 못했다 —
(a) 검증 요구 절의 서술형 항목 대부분(경쟁 처리 순서, atomic commit 경계, race 처리, replay
idempotency 등) — 세션 라운드에서 발견된 "tombstone 순서/lifecycle-generation 비교/held-queue
cap" 같은 순서 위반 부류를 이 라운드에서 전수 재현하지 못했다. `runtime/host/service-relocation-host-runtime.ts`(3,200+줄)와
`runtime/actors/actor-join-relocation.ts` 등 relocation/Join 순서 로직은 표층 존재만 확인했고
단계별 순서를 line-by-line 대조하지 않았다.
(b) `framework/languages/node/test/contract`의 개별 assertion 내용(파일 존재만 확인, 각 케이스의
관찰값까지는 미확인).
(c) `stream-connector`, `framework-codec-*`, `framework-locations-redis` 패키지.
(d) message-flow-tracing·flow-correlation의 attribute key/phase/reason 닫힌 값 목록 전수 대조.
(e) wire 프로토콜 command 47/48/49/44의 필드 단위 대조(command 번호 자체는
`service-wire-constants.generated.ts`로 확인 가능하나 이번 라운드에서 열지 않음).

## 주제별 대조

### 00-foundation

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| `ErrorKind` 13개, 0~12 | 일치 | `framework/languages/node/packages/framework/src/contracts/Errors/ZLinkFrameworkException.ts:2-16` | `NotFound=0`…`InternalFailure=12`, 13개 정확히 일치 |
| 재시도 hint 부재(공개 오류 표면) | 일치 | 위 파일 전체에 `retryable`/`retryAfter`/`RetryHint` 필드 없음(grep 무결과) | `ZLinkFrameworkException`은 `kind`·`message`·`cause`만 보유 |
| binding 타입이 public contract signature에 노출되지 않음(§7 정적 검사) | 판단 불가 | — | 이번 라운드에서 미확인 |
| Send/Request 완료 경계, 늦은 reply 무시 | 판단 불가 | — | 세션 주제에서 이미 유사 항목(R#) 확인됨, 이번엔 foundation 관점 재확인 안 함 |

### 01-execution

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| Application job queue pressure: pause 80% / resume 60% | 일치 | `framework/languages/node/packages/framework/src/runtime/host/application-job-queue.ts:60-61` (`DEFAULT_PAUSE_THRESHOLD_PERCENT = 80`, `DEFAULT_RESUME_THRESHOLD_PERCENT = 60`) | |
| Actor Join 제한: handler당 64개, request 1 MiB, 합계 8 MiB | 일치 | `framework/languages/node/packages/framework/src/runtime/actors/actor-join-deferred-scope.ts:5-7` (`MAX_DEFERRED_JOINS=64`, `MAX_DEFERRED_JOIN_REQUEST_BYTES=1024*1024`, `MAX_DEFERRED_JOIN_BYTES=8*1024*1024`) | |
| Actor Join 기본 timeout 5초 | 일치 | `framework/languages/node/packages/framework/src/runtime/actors/actor-context.ts:171,308` (`private timeoutMs = 5_000`) | |
| Overrun policy 3종(SkipLateTicks/CatchUpBounded/DelayNextTick)과 tick 계산 | 일치 | `framework/languages/node/packages/framework/src/contracts/Timers/ZLinkTimerOptions.ts:8-10`; `runtime/spots/spot-timer.ts:307-377`(`selectScheduledIndex`가 정책별 분기) | `SkippedTicks = scheduledIndex - lastScheduledIndex - 1`(spot-timer.ts:322)가 스펙 계산식과 일치 |
| Weight 0/기본 100/상한 10000, -1·10001 거부 | 일치 | `framework/languages/node/packages/framework/src/runtime/channels/channel-socket-options.ts:141`, `runtime/channels/channel-socket-registry.ts:271,1584,1794`, `runtime/locations/location-store-repository.ts:4167` — 전부 `Weight must be an integer in 0..10000` | |
| Framework가 retained receive·`send_ready` waiter·별도 send retry를 사용하지 않는다(부정 규칙) | 일치 | `runtime/` 전체 grep에서 `send_ready`/`SEND_READY`/`retainedReceive` 미검출 | |
| 수신 3축 상한(건수/byte/경과시간)의 정확한 값 | 스펙 미정(값은 확인) | `framework/languages/node/packages/framework/src/runtime/channels/channel-receive-loops.ts:807-821` (`RECEIVE_BATCH_MESSAGE_LIMIT=64`, `RECEIVE_BATCH_BYTE_LIMIT=4n*1024n*1024n`, `RECEIVE_BATCH_TIME_LIMIT_MS=2`) | G18과 동일 사실. 스펙은 값을 정하지 않음(05-transport-liveness §10) — node 값: 64건/4MiB/2ms |
| 송신 codec 선택 cache 1,024개 상한(내부 확인 조건) | 판단 불가 | — | `runtime/codecs/index.ts`에서 1024 상수 미검출, 정적 코드 구조만으로 판단 어려움 |

### 02-channel-transport

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| liveness probe 5초 / peer deadline 15초 | 일치 | `framework/languages/node/packages/framework/src/runtime/channels/channel-socket-registry.ts:58-60` (`CLIENT_SERVER_PROBE_INTERVAL_MS=5_000`, `CLIENT_SERVER_PEER_DEADLINE_MS=15_000`) | |
| Weight 0/100/10000/-1/10001 (topology 레벨) | 일치 | `runtime/locations/location-store-repository.ts:4167`, `runtime/locations/in-memory-location-store.ts:1277` | 01-execution 항목과 동일 코드 경로 |
| Connection projection/snapshot 형태의 공개 monitoring API 존재 여부(G21) | 불일치 후보 → 아래 G21 참고 | `framework/languages/node/packages/framework/src/contracts/`에 `ConnectionSnapshot`/`ConnectionStatus` 없음; `runtime/backend/node/node-monitor-backend-adapter.ts`는 Core 소켓 monitor를 감싸는 내부 어댑터일 뿐 application 공개 표면이 아님 | |
| RID/Spot ID 형식(prefix+lowercase UUID v4), fixed RID·automatic discovery 동시 설정 금지 등 | 판단 불가 | — | 이번 라운드 미확인 |

### 03-spot-actor

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| Actor 생성 종결 상태 3종 이름(G19) | `failed` | `framework/languages/node/packages/framework/src/contracts/Locations/Authority.ts:157` (`export type ZLinkCreationTerminalState = 'created' \| 'rejected' \| 'failed'`) | 스펙 문서 간 `Failed` vs `Aborted` 불일치(G19)에서, node는 `failed`를 사용 — 14 §6.4의 `Failed` 표기와 일치, 15의 `Aborted` 표기와는 불일치 |
| Actor Join 제한(64/1 MiB/8 MiB/5초) — membership 문서 관점 재확인 | 일치 | 01-execution 표와 동일 근거(`actor-join-deferred-scope.ts:5-7`, `actor-context.ts:171,308`) | |
| Actor 업무 payload가 Spot callback/Spot application queue를 거치지 않음(부정 규칙) | 판단 불가 | — | dispatch 경로 구조까지는 확인했으나(`runtime/spots/spot-actor-packet-drain.ts` 등 존재) payload가 실제로 Actor queue로 직접 가는지 라인 단위 추적은 하지 않음 |

### 05-location-relocation

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| 이동 대상 목록 한 페이지: 최대 1,024개, 최대 1 MiB(G20) | 일치 | `framework/languages/node/packages/framework/src/runtime/locations/aggregate-inventory-store.ts:14-15` (`MAX_PAGE_ENTRIES=1_024`, `MAX_PAGE_BYTES=1_024*1_024`) | 스펙 `05-location-relocation/01-location-runtime.ko.md:78,1018`의 "1,024개·1 MiB"와 정확히 일치 |
| CAS batch unique key 상한 2,048개(G20 비교 대상) | 일치 | `framework/languages/node/packages/framework/src/runtime/locations/in-memory-provider-location-store.ts:177` (`keys.length > 2_048`) | 이동 목록 페이지(1,024)와 CAS batch key 수(2,048)는 서로 다른 개념 — 재구성판 스펙(`02-location-store-redis.ko.md:111`)이 이미 두 값을 분리 서술해 G20의 원 문서 모순이 해소된 것으로 보임(사실만 보고, 판단은 보류) |
| Authority scan 페이지 상한 1,000개 | 일치 | `framework/languages/node/packages/framework/src/runtime/locations/location-store-repository.ts:220` (`Authority scan limit must be in 1..1000`) | `01-location-runtime.ko.md`의 "목록 읽기 한 페이지 최대 1,000개·4 MiB" 서술과 건수는 일치, 4 MiB 바이트 상한과 다음 페이지 토큰 4,096 bytes는 이번 라운드에서 미확인 |
| 세대(`ObjectGeneration` 등) `2^63-1` 고갈 시 `GenerationExhausted` | 일치(값 존재 확인) | `framework/languages/node/packages/framework/src/contracts/Locations/Authority.ts`, `contracts/Locations/Writes.ts`, `runtime/locations/in-memory-authority-store.ts` 등에 `GenerationExhausted` kind 정의 존재(grep 확인) | 정확한 `2^63-1` 경계값 비교 로직까지는 라인 단위로 확인하지 않음 |
| Terminal 결과 재생 유효시간 5분 | 일치 | `framework/languages/node/packages/framework/src/runtime/locations/in-memory-authority-store.ts:37`, `runtime/locations/location-store-repository.ts:92` (`CREATION_TERMINAL_RETENTION_MS = 5*60*1000`) | |
| Host relocation `Shutdown` 기본 deadline 30초 | 일치 | `framework/languages/node/packages/framework/src/runtime/host/route-mesh-runtime.ts:349` (`shutdownHost(deadlineMs = 30_000, ...)`), `runtime/host/service-relocation-host-runtime.ts:659,1940,3224` | |
| Session route update 기본 timeout 3,000ms | 일치 | `framework/languages/node/packages/framework/src/runtime/streams/actor-session-binding-registry.ts:154`, `runtime/host/remote-bound-session-relay.ts:339`, `runtime/streams/index.ts:384` (`sessionRelocationSealTimeoutMs ?? 3_000`) | |
| Relocation handoff chunk 보수 크기 32 KiB | 일치 | `framework/languages/node/packages/framework/src/runtime/host/relocation-direct-transfer.ts:13` (`RELOCATION_CONSERVATIVE_CHUNK_LIMIT_BYTES = 32 * 1024`) | |
| Interruption 목표 1초, in-flight budget pacing, 재전송 batch replace 등 순서 규칙 | 판단 불가 | — | `service-relocation-host-runtime.ts`(3,200+줄) 규모상 이번 라운드 예산 안에서 전수 대조 못 함 — 세션 라운드에서 발견된 순서 위반 부류(예: seal 전/후, cutover 전/후 abort 처리 순서)가 재현되는지는 후속 라운드 권장 |

### 06-observability

| 확인 항목 | 판정 | 근거 (파일:줄) | 비고 |
|---|---|---|---|
| Public Framework interface에 exporter/reader/storage/bucket/metric event DTO/connection snapshot 없음(부정 규칙) | 일치(부분) | `framework/languages/node/packages/framework/src/contracts/`에 해당 이름의 공개 타입 없음(grep 확인) | Metric label 닫힌 집합(topic/ActorId/SpotId/RID/endpoint/correlation/flow 제외) 자체는 이번 라운드에서 실제 label 생성 코드까지 추적하지 않음 |
| Metric·trace·flow-correlation의 정확한 이름 집합(event_id, phase, surface, message_kind, outcome, reason, action, attribute key) | 판단 불가 | — | 이번 라운드에서 미확인 — 우선순위 낮음으로 배제 |

## G 항목 — 이 구현의 실제 동작

| G# | 이 구현이 하는 것 | 근거 (파일:줄) |
|---|---|---|
| G18 | 수신 독점 상한 3축: 64건 / 4 MiB / 2ms 중 먼저 닿는 조건으로 batch를 끊고 `yieldAndReset()`으로 `setImmediate` 뒤 재시작 | `framework/languages/node/packages/framework/src/runtime/channels/channel-receive-loops.ts:807-834`(`RECEIVE_BATCH_MESSAGE_LIMIT=64`, `RECEIVE_BATCH_BYTE_LIMIT=4n*1024n*1024n`, `RECEIVE_BATCH_TIME_LIMIT_MS=2`, `record()`/`yieldAndReset()`) |
| G19 | Actor 생성 종결 상태 3종의 3번째 leaf 이름은 `'failed'`(`'created' \| 'rejected' \| 'failed'`) — `Aborted`라는 이름은 이 타입에 없음 | `framework/languages/node/packages/framework/src/contracts/Locations/Authority.ts:157` |
| G20 | 이동 대상 목록 한 페이지: 항목 최대 1,024개·바이트 최대 1 MiB(`aggregate-inventory-store.ts`). 이와 별개로 Location Store CAS batch의 unique key 상한은 2,048개(`in-memory-provider-location-store.ts:177`). 두 상한은 서로 다른 대상(페이지 목록 vs CAS batch)에 적용되는 별개 값으로 코드에 나타난다 | `framework/languages/node/packages/framework/src/runtime/locations/aggregate-inventory-store.ts:14-15`; `runtime/locations/in-memory-provider-location-store.ts:177` |
| G21 | Application 공개 계약(`contracts/`)에 connection projection/snapshot 타입이 없다. Monitoring 관련 코드는 `runtime/backend/node/node-monitor-backend-adapter.ts`의 Core raw socket monitor 어댑터뿐이며 이는 내부 backend 계약(`ZLinkBackendSocketMonitor`)이지 application-facing public API가 아니다 | `framework/languages/node/packages/framework/src/runtime/backend/node/node-monitor-backend-adapter.ts:1-28`; `framework/languages/node/packages/framework/src/contracts/`(해당 이름의 공개 타입 부재, grep 확인) |

## 요약

- 불일치 N건: 0건 — 이번 라운드에서 수치·닫힌 값·부정 규칙 확인 항목 중 스펙과 어긋나는 값은
  발견하지 못했다(위 표의 "일치" 항목은 모두 스펙 서술과 정확히 일치).
- 스펙 미정 N건: 1건 — 수신 3축 상한의 정확한 값(01-execution/05, 02-channel-transport/05
  §10에서 이미 "스펙이 정하지 않음"으로 명시). node의 실제 값은 64건/4 MiB/2ms(G18과 동일).
- 판단 불가 N건: 다수(00-foundation 2건, 01-execution 1건, 02-channel-transport 1건,
  03-spot-actor 1건, 05-location-relocation 1건, 06-observability 1건) — 서술형 순서·경쟁
  처리·atomic commit 경계 규칙과 event/attribute 닫힌 값 목록 등, 이번 라운드 예산으로는 원본
  코드를 라인 단위로 추적하지 못한 항목. 특히 05-location-relocation의 relocation
  handoff 순서 규칙(`service-relocation-host-runtime.ts`)은 세션 라운드에서 node가 순서 위반
  7건 중 다수를 낸 영역과 겹치므로, 후속 라운드에서 우선 재검토를 권장한다.

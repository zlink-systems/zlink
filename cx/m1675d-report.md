# m1675d — Node #1675 메시지당 비용 2차 보고

브랜치 `framework-node/1675-per-message-cost-2`(worktree `/root/worktree/zlink-1675-node-2`). 시작할 때 `origin/main`을 merge했다(`564ec281f1`, 충돌 없음). 커밋은 6개다. 항목마다 하나씩이고, M6A codec 서식 수정이 하나 더 있다. push·PR은 하지 않았다.

| 항목 | 커밋 | 요약 |
|---|---|---|
| 1 S2 flow 생성 조건 | `2f9c5f4ca7` | flow ID는 Normal·Detailed에서만 만든다. 판정은 `messageFlowTracingEnabled` 하나다 |
| 2 pump Promise | `02dfc65e22` | record마다 쓰던 `.then` 래퍼를 없앴다. 허가가 있는 경로와 없는 경로의 dispatch 사본을 합쳤다 |
| 3 M6A codec | `3c1540e30a`, `ecd88da442`(서식) | header를 한 번만 할당한다. text는 frame 안에서 바로 쓰고 읽는다. backend의 두 번째 decoder를 지웠다 |
| 4 correlation id | `28ef820303` | process prefix와 counter로 만든다(Java와 같은 모양) |
| 5 encoded payload | `2bc745465b` | 수신 쪽 serializer 입력에서 복사를 없앴다(기존 내부 helper를 재사용) |

결정이 필요한 것이 셋이다. §5에 정리했다(request-result 구조, pump의 다음 단계, 공개 `ZLinkEncodedPayload` 경로).

## 1. 계수 방법

- runner: `/root/jobs/fx/cx/m1675d/coefficients.cjs`. m1675c runner를 복사해 계수 세 가지를 더했다.
  - `AsyncLocalStorage.prototype.run` 호출 수(`ALS-run`)
  - 명시적 `Promise.prototype.then` 체인 수(`then`). `catch`·`finally`도 포함한다
  - `Buffer.prototype.subarray/slice` view 수(`view`)
- 구성: NestJS host 두 개를 한 process에 띄운다. RouteMesh node-direct, payload 1024 B다. 패턴은 send, request-serial, request-backpressure(동시 8)이고 각각 128건이다. 값은 두 host 합계를 메시지 하나로 나눈 것이다.
- 시간은 재지 않았다. `PROMISE`는 async_hooks로 센 값이라 `await` 하나마다 hook 전용 throwaway Promise 하나가 더해진다. 그래서 pump의 실제 Promise 수는 구조로 따로 적는다(§2 항목 2).
- 기준(base)은 `/root/worktree/zlink-1675-node`(`18d78e3dad`)다. 이번 merge 뒤 node 코드에는 차이가 없다.
- 원자료: `/root/jobs/fx/cx/m1675d/{base,i1..i5}-{count,heap}.json`, `*-summary.txt`. 미세 계수 script는 `work/request-result-alloc.cjs`, `work/correlation-alloc.cjs`다.

## 2. 항목별 전/후

### 전체(메시지당, 1024 B, 두 host 합계)

| pattern | 지표 | base | 최종 |
|---|---|---:|---:|
| send | Buffer factory / bytes | 11 / 6502 | 5 / 2379 |
| send | Buffer view | 5 | 3 |
| send | ALS run | 2 | 0 |
| send | 명시적 then | 3.04 | 2.04 |
| send | PROMISE(hook) | 6.79 | 5.79 |
| request-serial | Buffer factory / bytes | 37 / 13415 | 12 / 5298 |
| request-serial | Buffer view | 11 | 9 |
| request-serial | ALS run | 2 | 0 |
| request-serial | 명시적 then | 8 | 7 |
| request-serial | PROMISE(hook) | 29.0 | 28.0 |
| request-backpressure | Buffer factory / bytes | 37 / 13415 | 12 / 5298 |
| request-backpressure | PROMISE(hook) | 19.8 | 18.8 |

- macrotask(Immediate+Timeout)는 count 모드에서 send 0.13, serial 0, bp 0.09다. base와 같은 수준이다. 이번 항목은 이 값을 바꾸지 않았다.
- request-serial의 Buffer 12개는 Framework 8개와 binding 4개(`routing_id`·`router_socket`)다. Framework 8개는 이렇다.
  - M6A frame 2개와 routing·reply header 2개. wire 형식이 요구하는 쓰기다
  - JSON channel header 2개
  - 송신 쪽 protobuf serializer의 공개 `from()` 복사 2개(§5-3)

### 항목 1 — S2 flow ID 조건(`2f9c5f4ca7`)

- **소유 계층:** diagnostics. message-flow tracer가 수준을 판정한다.
- **스펙:** `06-observability/04-flow-correlation.ko.md:91`(spec 브랜치). Normal·Detailed일 때만 tracing이 켜져 있다. flow field 검증은 :68-69, 비용 규칙은 `03-message-flow-tracing.ko.md:201`이다.
- **교차언어:**
  - C++(`zlink-1639-msg-cpp`)은 `flow_context.hpp`의 `flow_context_t::tracing(mode)` 하나로 이미 Normal·Detailed만 허용한다.
  - Java(`ZLinkMessageFlowTracer.java:104`)와 .NET은 main에서 아직 `!= Off`다. 그 언어들의 #1639 작업 범위다.
- **분류:** A(스펙 개정에 맞춤).
- **변경:** `diagnostics/message-flow.ts`에 `messageFlowTracingEnabled(mode)`를 추가했다. host `flowCreationEnabled`와 tracer `flowCreationEnabled`가 모두 이 함수를 읽는다. 판정 위치는 2(`mode !== 'off'` 두 곳)에서 1로 줄었다. `channel-transports.ts:332-348,425-436`의 송신별 outbound ALS는 기본 수준(Errors)에서 이 판정으로 생략된다.
- **시험:** `message-flow.test.js` MFLOW-MAL 2건은 flow field 검증을 전제한다. 이 2건만 Normal로 옮겼고 스펙 줄(:68-69, :91)을 주석에 적었다. assertion은 바꾸지 않았다. flow·messageFlow를 언급하는 시험 22개 파일을 돌렸고 다른 실패는 없었다.
- **계수:** 송신·요청마다 ALS run 2 → 0. send Buffer 11 → 10(6502 → 6030 B), request-serial Buffer bytes 13415 → 12911.

### 항목 2 — dispatch pump(`02dfc65e22`)

- **소유 계층:** dispatch pump(application worker의 drain turn).
- **스펙:** `08-messaging-hot-path.ko.md:224-225`. record마다 dispatch task나 supervisor wrapper를 만들지 않는다. 그 밖에 W1–W3(:219-221)와 §6 양보 규칙을 따른다.
- **교차언어:** .NET `ZLinkMeshDispatchPump.cs:355,414-450`은 claim마다 async 하나를 두고 handler 결과를 pending 목록에 둔다. record마다 래퍼를 만들지 않는다.
- **분류:** B.
- **변경:**
  - 비동기 handler의 결과를 drain turn이 직접 `await`한다. 그 뒤 같은 continuation에서 record를 마치고 permit turn을 반환한다. 전에는 `Promise.resolve(result).then(...)` 래퍼를 거쳤다.
  - `dispatchRecord`·`dispatchPermittedRecord`·`finishPermittedRecord` 세 사본을 `dispatchRecord`·`finishRecord`(permit은 선택)와 `failRecord`로 합쳤다.
  - permit turn을 반환하는 위치는 4곳에서 2곳이 됐다.
- **되돌린 시도:** 대기할 일이 이미 있을 때 `waitForApplicationReady`가 `Promise.resolve()`를 만들지 않게 해 봤다. 그러자 새로 시작한 worker가 수신 회전의 stack 위에서 첫 handler를 실행했다(`node-dispatch-turn.test.js` 1번 실패, W1–W3 위반). 이 resolved Promise가 turn 경계 역할을 하므로 그대로 두었다.
- **양보 동작:** 바꾸지 않았다. `backend-contract` "yields between continuous ready batches"는 단독 20/20 통과했다. 같은 파일 전체를 10개 process로 병렬 실행해도 20/20 통과했다.
- **계수:** pump의 명시적 then은 request-serial 1.04 → 0, send 1.06 → 0.03이다. pump PROMISE(hook)는 request-serial 8.1 → 7.0, send 2.7 → 1.4다.
- **실제 Promise 수:** 비동기 handler record 하나에 4 → 3이다. 남은 셋은 idle worker 대기 Promise, `drainDomain` async, `drainClaim` async다. 나머지 넷은 hook 전용 await다.

### 항목 3 — M6A codec(`3c1540e30a`, 서식 `ecd88da442`)

- **소유 계층:** service wire M6A codec(Framework). text 규칙은 `service-wire-binary-primitives.ts`가 소유한다.
- **스펙:**
  - `08-messaging-hot-path.ko.md:96` E1: wire 자리 하나에 한 번만 쓴다.
  - :191-192 I3/I4: header만 해석하고 복사 없이 이동한다.
  - `05-payload-ownership` §2.
- **교차언어:**
  - .NET `ZLinkApplicationPayloadEnvelopeCodec.cs:34-37,226-237`은 multipart 상수의 UTF-8 byte를 미리 만들어 두고 `SequenceEqual`로 비교한다.
  - C++ `service_wire_codec.cpp:2846`은 string_view로 비교한다.
- **분류:** B.
- **변경:**
  - routing header(nodeRequest, channelSend·Request)와 reply header를 한 번만 할당하고 그 자리에 쓴다. 전에는 prefix, u64, concat을 1~2번 거쳤다. reply header의 기본 빈 tail 할당도 없앴다.
  - application payload의 text는 `canonicalServiceWireTextLength`로 검증한다(byte 길이, NUL, `isWellFormed`). 그다음 frame에 바로 쓴다. `encodeCanonicalServiceWireText`도 같은 검증 함수를 쓴다. 검증 규칙의 소유자는 하나다.
  - decode는 fatal UTF-8 decoder(`ignoreBOM: true`)로 frame 안에서 바로 한다. 전에는 복사 1번과 재인코딩 비교를 했다. 받아들이는 입력 집합은 이전과 같다.
  - reply tail은 frame의 view로 돌려준다. 호출자는 하나(`raw-service-mesh-runtime.ts:1446`)이고 비어 있는지만 확인한다.
  - `decodeMultipartApplicationPayloadView`를 codec으로 옮겼다. packetName·contentType은 byte로 비교하고 문자열을 만들지 않는다. backend의 `decodeApplicationEnvelope`(같은 frame의 두 번째 decoder, 브리프의 `node-raw-mesh-backend.ts:2338`)는 지웠다. M6A application frame decoder는 2개에서 1개가 됐다.
  - 입력이 이미 Buffer이면 `asBuffer`와 multipart parser는 view를 만들지 않는다.
- **계수(항목 2 뒤 → 뒤):**
  - request-serial: Buffer 37 → 14(12911 → 7346 B), view 11 → 9. codec의 Buffer 16 → 4이고 primitives는 8 → 0이다.
  - send: Buffer 10 → 6, view 5 → 3.
  - codec에 남은 Buffer 4개는 payload frame 2개와 routing·reply header 2개다.

### 항목 4 — request-result와 correlation id(`28ef820303`)

- **소유 계층:** channel envelope(correlation 형식)과 messaging request-result.
- **스펙:** `06-observability/04-flow-correlation.ko.md:61`. `correlation_id`는 1..64 byte opaque ASCII이고, 같은 lifecycle에서 대기 중인 request 사이에 중복되면 안 된다.
- **교차언어:**
  - Java `ZLinkChannelEnvelope.newCorrelationId` → `ZLinkServiceOperationIds`: process prefix 64-bit + counter, 32 hex.
  - C++ `client_call_codec.cpp:17` `next_correlation_id`: counter hex.
- **분류:** B.
- **변경:** correlation id를 process random 64-bit prefix와 process counter로 만든다. 둘 다 소문자 hex이고 합쳐 32자다. 준비된 channel header의 32자 자리를 그대로 쓴다.
- **계수:** id 하나당 heap 955 → 205~240 B(`work/correlation-alloc.cjs`). runner의 파일별 heap은 node 내부 crypto 할당을 세지 않아 이 차이가 보이지 않는다.
- **request-result:** 구현하지 않았다. 대안이 갈리는 설계라서 §5-1에 정리했다.

### 항목 5 — ZLinkEncodedPayload(`2bc745465b`)

- **소유 계층:** contracts `ZLinkEncodedPayload`(공개)와 runtime의 payload 경계.
- **스펙:**
  - `08-messaging-hot-path.ko.md:96` E1과 05 §2: Framework가 더하는 복사.
  - `bindings/doc/spec/message-ownership.ko.md:494`: Node receive는 JS-owned Buffer다.
- **교차언어:**
  - .NET `ZLinkEncodedPayload.cs:33,40`은 `internal FromOwned`를 둔다. protobuf codec이 `InternalsVisibleTo`로 이를 쓰고(`ZLinkProtobufCodec.cs:132`), 읽을 때는 공개 `Bytes`(ReadOnlyMemory, 복사 없음)를 쓴다.
  - Java `ZLinkEncodedPayload.java:15,19`는 양쪽 모두 복사한다.
- **분류:** B(수신 쪽). 송신 쪽은 공개 계약에 걸려 있어 §5-3에서 결정을 받는다.
- **변경:**
  - `decodeChannelPayload`와 channel reply decoder가 serializer에 넘기기 전에 공개 `from()`으로 복사하던 것을 바꿨다. 이제 `payload-codec.ts`의 기존 `encodedPayloadFromOwned`를 쓴다. 이 helper는 receive Buffer를 복사 없이 넘기는 이미 있던 경로이고, 이번에 export만 했다.
  - 같은 helper 안의 `from(Buffer.alloc(0))` 뒤 storage 교체를 직접 adopt로 바꿨다. 빈 Buffer 2개를 덜 할당한다.
  - 공개 `from()`·`data()`는 여전히 복사한다. 의미 변화는 없다.
- **계수:** encoded-payload 복사는 send 2 → 1, request-serial 4 → 2다(Buffer bytes 7346 → 5298).
- **남은 것:**
  - 송신 쪽 `framework-codec-protobuf/src/framework.ts:92`의 `ZLinkEncodedPayload.from(encoded.payload)`
  - 수신 쪽 `payload.data()`(:97). Uint8Array 복사라 runner에는 잡히지 않는다.
  - 둘 다 패키지 경계를 넘는 공개 API 문제다(§5-3).

## 3. 시험

| 검사 | 결과 | 로그 |
|---|---|---|
| `npm run build` | 통과 | 각 항목 build 로그 |
| `npm test` runtime gate(build·typecheck·lint·전체 TAP) | 2702/2702 완료, 실패 1개 파일: `client-server-location-runtime` 22번(#1819) | `/root/jobs/fx/cx/m1675d/logs/final-test.log` |
| `verify:m6a-runtime` | 44/44 (gate가 exit 1이라 `&&` 뒤를 따로 실행) | `logs/final-m6a.log` |
| `scripts/format/format.sh --check node` | 통과 | `logs/final-format.log` |
| 항목마다 전체 시험 파일 211개 병렬 실행 | 항목 1~5 모두 실패 0(아래 예외 2건) | scratchpad `run-i*` |
| backend-contract 24번 | 단독 20/20, 10 process 병렬 20/20 | — |

- `client-server-location-runtime` 22번(liveness ACK)은 같은 머신에서 20회씩 돌려 비교했다. main-base(`61893d9ae3`) 2/20, PR #1809 브랜치(`zlink-1675-node`) 2/20, 이 브랜치 2/20으로 실패율이 같다. 기존 간헐 실패 #1819다.
- 항목 3 실행 중 같은 22번(liveness ACK)이 한 번 실패했다. main에서도 나는 간헐 실패 #1819와 같은 시험·assertion이고, 그다음 실행들에서는 통과했다.
- `unity-webgl-package` 2번은 build 산출물인 `.jspre`를 되돌린 상태에서만 실패한다. 커밋된 `.jspre`가 main의 stream-connector 소스보다 오래됐기 때문이다(main에서도 같다). gate는 build로 다시 만든 뒤 검사하므로 통과한다. `.jspre`는 커밋하지 않았다.

## 4. 판정 위치 수(전 → 후)

| 결정 | 전 | 후 |
|---|---:|---:|
| flow 생성 수준 | 2 | 1 |
| record 뒤 permit turn 반환 | 4 | 2(`failRecord`, `finishRecord`) |
| record dispatch 사본 | 2 | 1 |
| M6A application frame decode | 2(codec, backend) | 1 |
| service-wire text 정규 검증 | 1 | 1(길이 함수로 옮김, encode는 그것을 사용) |
| 수신 bytes를 serializer에 넘기는 방식 | 2(`from` 복사, `encodedPayloadFromOwned`) | 1 |

- 새로 더한 상태는 correlation counter와 prefix 하나다. 이것이 per-request `randomUUID` 호출을 대체한다.
- 새 cache, queue, lock, timer는 없다.
- multipart 상수 byte 두 개는 module 상수다. .NET도 같은 상수를 둔다.

## 5. 결정이 필요한 것

### 5-1. request-result(`messaging/request-result.ts:55`) — 대안 선택

요청 하나당 closure 5개(confirm, fail, observe, then 두 개), `then` Promise 하나, `defineProperty`(descriptor 객체와 accessor)가 든다. 미세 계수로 잰 floor(같은 await 구조)를 뺀 추가 heap은 아래와 같다(`work/request-result-alloc.cjs`).

| 대안 | 추가 heap/요청 | 공개 동작 |
|---|---:|---|
| 현재 | 515 B | — |
| R-a 상태 객체와 prototype 메서드(closure 4: start callback, then 2, getter) | 420 B | 같음 |
| R-b `Promise` 하위 클래스에 prototype `admitted` getter(species = Promise) | 414 B | reply의 constructor가 달라진다(`instanceof`는 그대로) |
| R-c `admitted`를 읽을 때만 then 관찰자를 붙임(읽지 않으면 then·closure 2·Promise 없음) | 미측정. bench는 매 요청 `admitted`를 읽는다 | 응답이 끝난 뒤 `admitted`를 읽으면 microtask 한 번 늦게 settle된다 |

- 추천은 R-a다. 공개 동작이 같다.
- R-c는 읽지 않는 호출자에게 가장 싸지만, settle 시점이 관찰 가능하게 달라진다.

### 5-2. pump의 다음 단계(`mesh-dispatch-pump.ts`)

비동기 handler record 하나에 남은 실제 Promise는 3개다(대기 Promise, `drainDomain`, `drainClaim`).

- **P-a:** application worker의 drain을 worker loop 안으로 옮기고, `drainDomain`은 infrastructure만 맡게 나눈다. 1개가 줄고 `receiveBatch !== undefined`·`claimBudget` 분기도 줄어든다. 다만 양보·`beginDrain` 관리 코드를 두 경로가 공유하도록 다시 짜야 한다.
- **P-b:** `drainClaim`이 handler가 pending일 때까지 동기로 돌고 `void | Promise`를 반환하게 한다. 동기 handler에서만 1개가 준다.
- 어느 쪽이든 W1–W3 turn 경계와 backend-contract 24번을 다시 검증해야 한다.

### 5-3. 송신 쪽 `ZLinkEncodedPayload` 복사 — 공개 계약이 걸림

- `framework-codec-protobuf`(별도 package)는 `@zlink-systems/framework`의 공개 API만 import한다. framework `package.json`의 exports는 `"."` 하나뿐이다.
- 복사를 없애려면 아래 셋 중 하나가 필요하다. 셋 다 공개 표면이 바뀐다.
  - **E-a:** 공개 `ZLinkEncodedPayload.fromOwned(bytes)`(소유권 이전, 이후 변경 금지)와 읽기 전용 view 접근자. .NET의 `FromOwned`와 `Bytes`에 해당하며 05 스펙 개정이 필요하다.
  - **E-b:** framework에 package 내부용 subpath export(예 `@zlink-systems/framework/internal`)를 두고 codec package만 쓴다. .NET `InternalsVisibleTo`에 해당하지만 Node에서는 누구나 import할 수 있다.
  - **E-c:** protobuf·msgpack의 framework serializer adapter를 framework package 안으로 옮긴다.
- 효과: send에서 Buffer 1개와 data() 복사 1개가 줄고, request-serial에서는 각각 2개가 준다.

## 6. 리팩토링 점검

리팩토링 점검: 성능 7건·POSDDD 3건·불필요 코드 4건 발견.

- **고친 것:**
  - 위 5개 항목.
  - pump의 dispatch 사본 3개 → 1개.
  - backend의 M6A decoder 중복 삭제.
  - backend의 미사용 `FATAL_UTF8`, `APPLICATION_PAYLOAD_*` import 정리.
  - `encodedPayloadFromOwned`의 빈 Buffer 2개 할당 제거.
- **넘긴 항목(`file:line`):**
  - `contracts/Common/ZLinkEncodedPayload.ts:13`, `framework-codec-protobuf/src/framework.ts:92,97` → §5-3.
  - `messaging/request-result.ts:55` → §5-1.
  - `backend/mesh-dispatch-pump.ts:159-190,315-460` → §5-2.
  - `backend/node/node-raw-mesh-backend.ts:2612` `encodeMultipart`: stateful 경로에서 multipart payload를 따로 만든 뒤 `encodeApplicationPayload`로 한 번 더 복사한다. node-direct hot path 밖이다.
  - `FATAL_UTF8` 사본: `service-authority-payload-codec.ts:15`, `service-instance-activation-recovery-codec.ts:12`, `service-metadata-codec.ts:2`, `actor-authority-payload-codec.ts:27`. 범위 밖 중복이다.
  - `admission.ts:103`: 비동기 handler 하나당 continuation 1개.

## 7. 스펙 개정 필요

없다. 항목 1은 spec 브랜치(PR #1821)의 개정을 그대로 따랐다.

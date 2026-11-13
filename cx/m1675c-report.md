# m1675c — Node #1675 메시지당 비용 감축 완료 보고

승인된 P1~P13을 모두 구현했다. 커밋은 13개이고 브랜치는 `framework-node/1675-per-message-cost`(시작 `90aafafaff`, 끝 `9de2a4a3fa`)다. 주 변경 11개와, 전체 시험에서 드러난 문제를 고친 후속 커밋 2개(P13 재작업, P1 실패 경로)로 이뤄진다. 시간은 재지 않았고 효과는 횟수와 할당량으로만 판정했다. Node 단위·contract 전체 시험과 format 검사에서 main 대비 새 실패는 0건이다. push·PR·보드 작업은 하지 않았다.

## 1. 계수 방법

- runner: `/root/jobs/fx/cx/m1675c/coefficients.cjs`(커밋하지 않음). 공개 NestJS host 두 개를 한 process에 띄우고(`framework/bench/grpc/node`와 같은 구성, codec은 payload를 그대로 쓰는 protobuf envelope serializer), 1024B로 send 128건, request-serial 128건, request-backpressure(동시 8) 128건을 처리한다.
- count 모드: `async_hooks` PROMISE·Immediate·Timeout 생성 수와 callback 수, Buffer factory 호출 수와 바이트 수를 생성 위치의 첫 runtime 파일별로 센다.
- heap 모드: HeapProfiler sampling(간격 128B, GC로 수거된 객체 포함)으로 runtime·binding 파일의 할당 byte를 센다. sampling 값이라 실행마다 몇 백 B씩 흔들린다.
- plain 모드: 계측 부담 없이 pump 양보 횟수와 setTimeout 수만 센다. count 모드는 stack을 잡느라 처리가 느려져 시간 조건(2ms 예산)이 왜곡되므로, 시간에 걸린 항목은 plain 모드로 판정했다.
- 항목별 미세 계수: `encode-alloc.cjs`(header encode 할당), `decode-alloc.cjs`(header decode 할당), contract 시험 안의 계수.
- 원자료: `/root/jobs/fx/cx/m1675c/*-count.json`, `*-heap.json`, `*-summary.txt`, `plain-*.json`. 로그는 `/root/jobs/fx/cx/m1675c/logs/`에 있다.
- "other"는 runner 자신의 대기 Promise다(메시지당 약 1.2~3.2).

## 2. 전체 전/후(1024B, 두 host 합계, 메시지당)

| pattern | 지표 | binding 경로 | main(수정 전) | 최종 |
|---|---|---:|---:|---:|
| send | PROMISE | 2.05 | 12.47 | 7.20 |
| send | Timeout | 0 | 0.375 | 0.078 |
| send | Buffer / bytes | 2 / 1034 | 13 / 7889 | 11 / 6502 |
| send | sampled JS heap B | 1299 | 10900 | 8182 |
| request-serial | PROMISE | 14.53 | 51.59 | 32.20 |
| request-serial | Timeout(count 모드) | 0 | 3.80 | 0.80 |
| request-serial | Buffer / bytes | 3 / 2058 | 40 / 14874 | 37 / 13415 |
| request-serial | sampled JS heap B | 4384 | 33103 | 22930 |
| request-backpressure | PROMISE | 11.30 | 32.82 | 20.51 |
| request-backpressure | sampled JS heap B | 4022 | 22227 | 15883 |

timer 콜백은 plain 모드에서 request-serial 메시지당 4.88(P6 시점) → 0.016으로 줄었다. binding 경로 값에는 binding 내부 `completion_owner` Promise 4와 runner 대기 Promise가 들어 있다.

## 3. 항목별 전/후

| 항목 | 커밋 | 수정 전 | 수정 후 | 근거 |
|---|---|---:|---:|---|
| P2·P9·P11 ingress 재구성·복사·reply route·배열 literal | `4480c2eb84` | send Buffer 13, 7889B, heap 10900B; payload 복사 1, reply당 RoutingId 생성 1 | 11, 6502B, 10105B; 복사 0, RoutingId 0 | runner, `node-ingress-payload-ownership.test.js` |
| P1 dispatch Promise 사슬 | `59d7e4ff7f`, `9de2a4a3fa` | send PROMISE 12.17, serial 52.9; one-way 경로의 Framework Promise 6.72 | 7.40, 47.6; 0.75 | runner, `node-dispatch-turn.test.js` |
| P3 request pending 4겹 | `a801356451` | serial PROMISE 47.6, bp 28.4; heap 28067/19386B | 40.8, 21.0; 26464/17270B | runner, `node-request-completion-path.test.js` |
| P6 회전 permit 저장소 | `36830ce5e1` | 1건 회전당 permit 상태 전환 192 | 3 | `node-request-rotation-cost.test.js` |
| P7 pump 양보 예산 | `9f1162c136` | pump timer 양보 serial 0.62, bp 0.15 | 0, 0.04 | plain 모드, `mesh-dispatch-yield-claim.test.js` |
| P4 flow 있는 준비된 header | `28ba4c24f7` | send+request header 한 쌍 할당 2485B, flow id 생성 813B | 1362B, 21B | `encode-alloc.cjs`, `node-ingress-header-encoding.test.js` |
| P5 inbound header decode | `6064566032` | decode당 1318B, 필드마다 JSON.parse, `isUtf8` 1회 | 601B, JSON.parse 0, `isUtf8` 0(ASCII canonical) | `decode-alloc.cjs`, 같은 시험 |
| P8 deadline 기반 관리 작업 | `7a4cb0053e` | 연결된 idle 한 쌍 200ms 동안 maintenance timer 256; serial timer 콜백 3.86 | 1; 0 | `node-dispatch-turn.test.js`, plain 모드 |
| P10 manual target 분류 | `b1154c23e6` | send당 resolver heap 151B | 0B | runner heap |
| P13 JSON bytes base64 | `b2058989e7`, `de593e8619` | 1024B 왕복 문자열 연산 5129 | 2057 | `framework-json-bytes-codec.test.js` |
| P12 completion table 확장 | `93139407c2` | 1024건까지 늘리는 동안 bucket 배열 생성 17 | 6 | `mesh-completion-table-growth.test.js` |

### 감독 조건 확인

**P1.** handler와 continuation이 새 execution turn에서 실행된다는 조건(01 §11, 08 §4.3·§4.4)을 시험으로 확인했다.
- `node-dispatch-turn.test.js`는 실제 RouteMesh host 한 쌍에서 handler가 실행되는 순간에 `pumpBatch`·`tickLiveness`가 실행 중이 아님을 확인한다. handler의 stack에 `pumpBatch|receiveOne|processReceived|tickLiveness|onReadable` frame이 없는 것도 확인한다.
- 같은 파일에서 동기 throw, throw, reject, await하는 handler 뒤에도 send와 request가 계속 처리되는 것을 확인한다.
- handler는 여전히 application worker의 drain turn에서 호출된다. 이번 변경은 그 turn 안의 wrapper만 걷어냈다.
- 전체 시험에서 closed admission과 dispatcher 미등록 경로가 동기 throw로 바뀐 것이 드러났다(`location-host.test.js` 2건). 후속 커밋 `9de2a4a3fa`에서 두 경로를 비동기 rejection으로 되돌렸다.

**P3.** 남긴 completion table 자리가 등록과 원자적 꺼내기 규칙(08 E3, 01 §10)을 그대로 지키는지 확인했다.
- 등록과 close의 직렬화: table slot은 submit 전에 잡히고 operation id가 반환된 뒤 색인된다(`mesh-completion-table.ts`의 기존 `submit`).
- 원자적 꺼내기: raw operation registry의 `take`(`operation-registry.ts`의 complete·fail) 한 곳에서 terminal 권한이 확정되고, observer가 같은 turn에서 table slot을 완료한다.
- 요청 호출 안에서 결정된 terminal(대상 없음, 동기 submit 실패, channel 미선택)은 id가 반환된 뒤에 전달된다. 이 순서는 `node-request-completion-path.test.js` 1번 시험으로 확인했다.
- 응답·timeout·close가 각각 정확히 한 번 전달되는 것도 같은 파일에서 확인했다.
- local 요청과 다른 호출자는 기존 Promise 계약을 유지한다.

**P8.** 관리 작업을 deadline 기반으로 바꾼 뒤에도 08 §4.2를 지킨다.
- 관리 작업은 회전당 최대 한 번이다. liveness tick은 수신 회전마다 한 번 호출되던 자리 그대로이고, due 전에는 일 없이 반환한다.
- I0 대기 상한은 가장 가까운 deadline이다. 이 deadline은 요청 만료, liveness probe·timeout, handshake가 남은 expected peer 가운데 가장 이른 것이다.
- handshake가 남은 expected peer는 기존 1ms announce 주기를 유지한다.
- 같은 파일의 deadline 단위 시험으로 확인했다.

## 4. 변경 파일

- `framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-binding-port.ts`
- `framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts`
- `framework/languages/node/packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts`
- `framework/languages/node/packages/framework/src/runtime/foundation/operation-registry.ts`
- `framework/languages/node/packages/framework/src/runtime/foundation/service-liveness-registry.ts`
- `framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-runtime.ts`
- `framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-registry.ts`
- `framework/languages/node/packages/framework/src/runtime/backend/mesh-dispatch-pump.ts`
- `framework/languages/node/packages/framework/src/runtime/backend/mesh-completion-table.ts`
- `framework/languages/node/packages/framework/src/runtime/application-jobs/application-job-queue-scope.ts`
- `framework/languages/node/packages/framework/src/runtime/admission.ts`
- `framework/languages/node/packages/framework/src/runtime/spots/spot-node-runtime-manager.ts`
- `framework/languages/node/packages/framework/src/runtime/channels/channel-dispatch-pipeline.ts`
- `framework/languages/node/packages/framework/src/runtime/channels/channel-dispatchers.ts`
- `framework/languages/node/packages/framework/src/runtime/channels/channel-runtime-lifecycle.ts`
- `framework/languages/node/packages/framework/src/runtime/channels/channel-runtime-manager.ts`
- `framework/languages/node/packages/framework/src/runtime/channels/channel-transports.ts`
- `framework/languages/node/packages/framework/src/runtime/channels/channel-clients.ts`
- `framework/languages/node/packages/framework/src/runtime/channels/channel-envelope.ts`
- `framework/languages/node/packages/framework/src/runtime/diagnostics/flow-context.ts`
- `framework/languages/node/packages/framework/src/runtime/host/application-job-queue.ts`
- `framework/languages/node/packages/framework/src/runtime/host/mesh-router-resolver.ts`
- `framework/languages/node/packages/stream-wire/src/internal/framework-json-v1.ts`

시험(새 파일): `node-dispatch-turn.test.js`, `node-request-completion-path.test.js`, `mesh-router-manual-target.test.js`, `framework-json-bytes-codec.test.js`, `mesh-completion-table-growth.test.js`.

시험(추가 또는 수정): `node-ingress-payload-ownership.test.js`, `node-request-rotation-cost.test.js`, `mesh-dispatch-yield-claim.test.js`, `node-ingress-header-encoding.test.js`, `operation-registry-management.test.js`. 모두 `framework/languages/node/test/contract/` 아래에 있다.

`operation-registry-management.test.js`는 fixture만 바꿨고 assertion은 그대로다. idle node가 deadline 없이 maintenance를 돌지 않게 되었으므로, 예산 분할을 관찰할 수 있게 짧은 timeout의 요청 하나를 추가했다. 분류는 A(계약 적응)다.

새 시험 가운데 회귀를 잡는 것들은 수정 전 코드로 build해 실패하는 것을 확인했다. 시험 이름, 수정 전 값, 수정 후 값은 다음과 같다.

| 시험 | 수정 전 | 수정 후 |
|---|---:|---:|
| payload 복사 | 1 | 0 |
| one-way Promise | 6.72 | 0.75 |
| 회전 전환 | 192 | 3 |
| idle 양보 | timer 1 | 0 |
| JSON.parse | 1+ | 0 |
| idle maintenance timer | 256 | 1 |
| base64 문자열 연산 | 5129 | ≤2100 |
| bucket 배열 | 17 | 6 |

## 5. 시험 결과

| 검사 | main(`90aafafaff`, 같은 머신) | 최종 |
|---|---|---|
| `npm test` runtime gate(build·typecheck·lint·전체 TAP) | 2686/2686 완료, 실패 파일 1개(`client-server-location-runtime` 22번 liveness ACK) | 2702/2702 완료, 실패 0, exit 0 |
| `verify:m6a-runtime` | 44/44 | 44/44 |
| `verify:m6b-runtime` | 131/132(117번 실패) | 131/132(같은 117번) |
| `verify:m6c-runtime` | 252/252 | 252/252 |
| `scripts/format/format.sh --check node` | exit 0 | exit 0 |

- main 대비 새 실패는 0건이다.
- `client-server-location-runtime` 22번은 main과 첫 최종 실행에서 같은 assertion으로 실패했다. 두 번째 최종 실행에서는 통과했으므로 부하에 따라 흔들리는 시험이다.
- m6b 117번(`raw backend dispatches Spot requests and Actor sends through M6B owners`)은 main에서도 같은 위치로 실패하는 기존 실패다.
- 첫 최종 실행에서 새로 나온 실패 4개 파일은 모두 처리했다. browser·Unity bundle의 `Buffer` 금지 2건은 `de593e8619`에서, location-host 2건은 `9de2a4a3fa`에서 고쳤다.
- 로그: `logs/base-*.log`, `logs/final-*.log`, `logs/final2-*.log`.

남은 실패: 위의 기존 실패 2건뿐이다. 이번 변경과 무관하며 main에서도 실패한다.

build할 때마다 `framework/languages/unity/com.zlink.stream-connector.webgl/Plugins/WebGL/zlink-stream-connector.jspre`가 Unity 동기화 단계에서 바뀐다. 수정 전 build에서도 같았으므로 커밋하지 않았다(작업 범위 밖).

## 6. 소유 계층·스펙·교차언어·분류

| 항목 | 소유 계층 | 스펙 조항 | 다른 세 언어의 대응 위치 | 분류 |
|---|---|---|---|---|
| P2·P9·P11 | Framework raw ingress; binding이 payload 저장소 소유 | 08 §4.1 I2–I4, 05 §2·§4, 08 W4 | C++ `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.hpp:142-162`(`_received` 재사용, `reply(request, parts)`), .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:5490-5500`, Java `.../runtime/binding/ZLinkJavaRawServicePort.java:373` | B |
| P1 | application worker의 drain turn | 08 §4.3 W1–W3, 02 §2·§4, 01 §11 | C++ `.../runtime/channels/route_packet_dispatcher.cpp`(동기 호출), .NET `.../Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:475`, Java `.../runtime/channels/ZLinkChannelRuntime.java:1387,1477` | B |
| P3 | raw operation registry(take) → Framework completion table(dispatcher 자리) | 08 E3–E5, 01 §10·§11 | .NET `.../Backend/DotNet/ZLinkMeshCompletionTable.cs:116,176`, C++ `.../runtime/foundation/operation_registry.hpp:38`, Java `.../internal/service/ZLinkServiceOperationRegistry.java:72,119` | B |
| P6 | host application job queue | 04-ajq §3·§4, 08 I1 | .NET `1648-receive-path` `1620452aa7`, main `ZLinkApplicationJobQueue.cs:339`; Java `ZLinkApplicationJobQueue.java:331`; C++ `.../runtime/dispatch/application_job_queue.hpp:385` | B |
| P7 | dispatch pump | 08 §4.3 W5·§6, 02 「9」 | .NET `.../Runtime/Execution/ZLinkExecutionLanePolicy.cs:6`, Java `.../execution/ZLinkSerialExecutionQueue.java:51,283`, C++ `.../runtime/dispatch/receive_batch_budget.hpp:13` | B |
| P4 | envelope codec; flow 소유는 diagnostics | 08 E1, 06-obs/04 §3·§4, 06-obs/03 §5 | .NET `.../Runtime/Channels/ZLinkChannelCalls.cs:19`·`ZLinkEnvelopeCodec.cs:174`, Java `ZLinkSpotRoutedOutbound.java:63`·`ZLinkChannelEnvelope.java:307`, C++ `.../diagnostics/flow_context.cpp:94`·`.../messaging/envelope_codec.cpp:350` | B |
| P5 | envelope codec | 08 §4.1 I3 | .NET `ZLinkEnvelopeCodec.cs:335-373`, Java `ZLinkChannelEnvelope.java:706-767`, C++ `envelope_codec.cpp:804-834` | B |
| P8 | raw backend 관리 작업 | 08 §4.2·I0 | Java `1648-receive-path` `7787607857`, .NET `ZLinkManagedMeshNode.cs:5346`, C++ host dispatch thread의 관리 deadline | B(fixture는 A) |
| P10 | host mesh router resolver | 08 E2, 02-channel-messaging 후보 목록 | .NET `.../Runtime/Spots/ZLinkSpotPeerConnectionSet.cs:113`, Java `ZLinkJavaRawMeshNode.java:1265-1344` | B |
| P13 | stream-wire framework-json-v1(browser 겸용) | 05 §2 | .NET `Convert.FromBase64String`, Java `Base64`(runtime 내장) | B |
| P12 | Framework completion table | 08 E3 | .NET `ZLinkMeshCompletionTable.cs`, C++ `operation_registry.hpp` | B |

### 공개 동작의 언어 간 차이

언어 사이에 공개 동작 차이를 새로 만들지 않았다. 세 가지는 다른 언어와 비교해 따로 적는다.

- P4의 flow id 생성 방식(RFC 9562 counter 방식)은 Node 내부 표현이다. 형식(소문자 UUIDv7, 36 byte)은 06-obs/04 §3 그대로다.
- P8에서 idle node는 1ms timer를 돌지 않는다. 이는 다른 세 언어가 이미 지키는 08 §4.2의 결과다.
- P13은 browser 겸용 package의 제약 때문에 portable 경로를 유지한다.

## 7. 판정 위치 수(수정 전 → 후)

| 결정 | 전 | 후 |
|---|---:|---:|
| mesh dispatch admission 해제 | 2(claim close·run then) | 1(`exitAdmission`) |
| record permit turn 반환 | 2(scope 내부, 반환 경로마다) | 1(`releaseApplicationJobPermitTurn`) |
| 원격 request terminal 전달 | 4(registry Promise → backend then → table → transport) | 2(registry take → table slot) |
| 회전 permit 예약 | 2(reserve 일괄·take) | 1(take) |
| pump 예산 시작 | 1 | 1 |
| maintenance 시점 | 1(고정 1ms) | 1(deadline 함수) |
| 그 밖의 항목 | 1 | 1 |

추가한 상태는 다음과 같다.
- registry의 earliest deadline. 하한 값이며, 전체 만료 검사를 마치면 정확한 값으로 바뀐다.
- liveness의 next due.
- pump의 draining 수.
- rotation의 idle 저장소 stack. 기존 저장소 배열을 대체한 것이며 새 pool이 아니다.

새 lock, queue, retry, cache는 없다.

## 8. 남은 예산 초과분(이번 범위 밖, file:line)

- `framework/languages/node/packages/framework/src/contracts/Common/ZLinkEncodedPayload.ts:5,13`와 `encoded-payload-storage.ts:3`: 공개 `ZLinkEncodedPayload`의 `from()`과 `data()`가 각각 payload 전체를 복사한다. 그래서 send마다 1024B Buffer 2개가 생기고 request-serial에서는 4개가 생긴다. 관련 위치는 `channel-envelope.ts:436`, `framework-codec-protobuf/src/framework.ts:92`다. 공개 계약의 복사 의미와 관련되므로 감독 판단이 필요하다.
- `mesh-dispatch-pump.ts:181-187,328,401`: worker가 깨어날 때마다 `new Promise`가 생기고, drain과 claim의 async 함수가 남아 있다. request-serial pump Promise는 메시지당 10.4다.
- `service-wire-m6a-codec.ts`: request-serial 메시지당 Buffer view 16. `node-raw-mesh-backend.ts:2338`: JSON header와 같은 packetName·contentType 문자열을 m6a에서 한 번 더 만든다.
- `messaging/request-result.ts:55`: 요청마다 closure 4와 `defineProperty`가 생긴다. `channel-envelope.ts:102`: correlation id를 만들 때 `randomUUID().replaceAll`.
- `admission.ts:83`: 비동기 handler 1건마다 continuation 1이 남는다.
- `channel-transports.ts:334,348,436`: 송신마다 outbound flow ALS `run`이 남는다.

## 9. 스펙 개정 필요

- 계획서 §5의 후보 하나만 있다. 기본 diagnostics 수준 `errors`에서 메시지마다 flow id를 만드는 비용이다(`spec/server/06-observability/04-flow-correlation.ko.md:80-98`).
- 감독자 지시대로 손대지 않았다. P4로 그 비용을 줄였을 뿐 동작은 그대로다.
- 이번 구현 중에 새로 생긴 스펙 개정 항목은 없다.

## 10. 리팩토링 점검

리팩토링 점검: 성능 13건·POSDDD 3건·불필요 코드 4건 발견.

- 고친 것:
  - 위 13개 항목.
  - permit turn 반환과 admission 해제를 소유자 한 곳으로 모음.
  - request 완료 단계를 registry take → table으로 정리.
  - 도달할 수 없던 submit 전 deadline 분기, `submitRequestOperation`, 회전 저장소 일괄 scan, 미사용 grant 인자 삭제.
- 넘긴 항목(§8): `contracts/Common/ZLinkEncodedPayload.ts:5,13`, `backend/mesh-dispatch-pump.ts:181-187`, `foundation/service-wire-m6a-codec.ts`, `messaging/request-result.ts:55`.

## 11. 감독 리뷰 반영(review-1675)

리뷰 지적 두 건(N1, N2)을 고쳤다. 다시 돌린 전체 시험에서 P7이 만든 결함이 하나 더 드러나 함께 고쳤다. 각각 커밋 하나이고 본문에 `Refs #1675`가 있다.

| 항목 | 커밋 | 내용 |
|---|---|---|
| N2 `isPromiseLike` 중복 | `fe501069f5` | `admission.ts`, `channel-dispatch-pipeline.ts`, `stream-session-runtime.ts`에 따로 있던 세 사본을 지우고 `runtime/abort.ts`(비동기 helper가 모인 기존 모듈)에 null 처리가 있는 형태 하나만 둔다. 세 곳은 그것을 import한다. 판정 위치 3 → 1. |
| N1 idle maintenance 시험의 실제 대기 | `35789ccc17` | `node-dispatch-turn.test.js`의 시험이 실제 200ms를 기다리지 않는다. 대신 `t.mock.timers`로 setTimeout을 가상 시간으로 돌린다. 연결 중에는 turn마다 1ms씩 진행하고, idle 가상 200ms 동안 걸린 maintenance timer가 2개 이하이며 모두 1초 이상 뒤의 deadline을 기다리는지 확인한다. 최종 코드: delay 4995ms 1개, 통과. `7a4cb0053e` 이전 코드: 1ms timer 400개, 실패(`logs/n1-red.log`). |
| 추가: pump 양보 공유 | `901ad46880` | 원인은 P7(`9f1162c136`)이 예산 시작 시점을 바꾼 것이다. 그 뒤로 예산을 쓴 drain 하나만 양보하고 다른 worker는 microtask로 계속 drain할 수 있었다. 증상은 `backend-contract.test.js` 24번 "yields between continuous ready batches"가 12회 중 4회 실패한 것이다(`9f1162c136` 이전 코드는 12회 모두 통과). 이제 공유 예산을 쓰면 모든 drain이 같은 macrotask 경계를 기다린다. 수정 후 20회 모두 통과했고, pump timer 양보는 메시지당 0.06 이하다(plain 모드). |

다시 돌린 결과(`logs/final4-*.log`):

| 검사 | 결과 |
|---|---|
| `npm test` runtime gate | 2702/2702 완료, 실패 0, exit 0 |
| m6a | 44/44 |
| m6b | 131/132(117번, main과 같은 기존 실패) |
| m6c | 252/252 |
| `format.sh --check node` | exit 0 |

main 대비 새 실패는 0건이다. Unity `.jspre`는 `git checkout --`로 되돌렸고 작업 트리는 clean이다. push는 하지 않았다.

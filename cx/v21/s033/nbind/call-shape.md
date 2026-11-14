# Node L1/L2/L3 message 호출 경로

기준 commit `dc2eef3023`, branch `exp/4350-node-binding-cost`. 코드 변경 없이 해당 worktree의 source와 새 dist를 사용한다. `F=framework/languages/node/packages/framework/src`, `B=bindings/node/src/zlink`, `N=bindings/node/native/src`, `T=framework/bench/grpc/node`. line은 이 commit 기준이다.

`→N`은 JS가 addon export를 호출하는 경계, `→JS`는 native가 JS allocator/callback을 호출하거나 결과 값을 반환하는 경계다. `napi_create_*`는 native 안의 JS 값 생성이며 별도의 JS→native 호출로 세지 않는다. 아래는 성공 경로다. 빈 receive/completion poll, route 조회, writable 재제출은 run별 별도 계수한다.

| 단계 | source submit | target receive | target reply submit (request만) | source completion receive |
|---|---|---|---|---|
| L1 raw | `T/shared/raw-wire.js:246` protobuf→`Message.from`; 고정 request header와 native body를 `T/client/main.js:259,304`에서 제출 | `T/zlink-framework-server/main.js:143` router recv→body `.data()`→protobuf decode. Header는 읽지 않는다. `T:231` permit을 받는 L1 loop도 같은 recv/body/reply 경로다 | `T:173` 새 protobuf `Message.from`, 고정 response header→`Received.reply().message().submit()` | `T/client/main.js:279` reply body만 읽고 parts close. Poller는 `T:218,230` |
| L2 envelope | 위 body와 `F/runtime/channels/channel-envelope.ts:124,156,471,560` header 생성. Request correlation `:114`, deadline `:521`, 준비 template `:658,709` 복사·field 쓰기. Send는 correlation 없는 template 재사용 `:708` | 같은 recv, `F/.../channel-envelope.ts:886,893` header `.data()`→canonical parser `:960,1052`→validation `:1624`→body decode. Header와 body를 모두 관찰 | `F/.../channel-envelope.ts:217` protobuf serializer와 reply header `:658`→같은 socketReply | 같은 completion, `F/.../channel-envelope.ts:886` response header도 parse. binding completion Buffer 수신 방식은 L1과 동일 |
| L3 service wire | L2 inner header/body 생성 후 `T/shared/raw-wire.js:174`가 native body `.data()` 노출, `F/runtime/foundation/service-wire-m6a-codec.ts:93/110` service head, `:216,256,274` multipart pack. Inner part를 새 JS frame에 각 1회 copy한 뒤 두 outer Buffer 제출 | outer header와 frame을 모두 `.data()`로 읽는다. `T/shared/raw-wire.js:199`→`F/.../service-wire-m6a-codec.ts:631,100/126,337,409`. Part Buffer view 2개+`F/runtime/backend/runtime-message.ts:16` wrapper 2개+결과 배열 1개. Inner channel header parse는 L2와 동일. Multipart decode byte copy 0 | `T/shared/raw-wire.js:187` L2 reply parts→service reply head `F/.../service-wire-m6a-codec.ts:143`+multipart pack `:216`→socketReply; body는 L2 reply serializer의 managed Buffer이므로 source 요청의 native body `.data()`와 다름 | 같은 completion으로 받은 outer parts를 `T/shared/raw-wire.js:222`→reply header `F/.../service-wire-m6a-codec.ts:164`·multipart view/parts `:337,409`→inner channel header parse·body decode |

Ladder switches는 요청한 값 그대로다. L1에만 `ZLINK_LADDER_L1_PERMIT=1`, L2는 envelope/client, L3는 service-wire/client를 켠다. L2/L3의 target loop는 L1 permit loop와 다르므로 이 비대칭을 codec 비용과 구분한다. Send에는 reply submit 및 payload completion receive가 없고, admission backpressure 때만 writable completion을 관찰한다.

## 모든 message 경계

| 동작 | binding JS→native export / native 근거 | copy·native→JS 값 생성 |
|---|---|---|
| protobuf body 소유화 | `B/contracts/messaging/message.ts:105,109` →N `messageFromBuffer`, `N/addon_core.cc:1055,1076`→`:211` | protobuf Buffer→native payload 전체 1회 copy. `N/addon_message_values.h:161,146`가 native handle+결과 객체 →JS 반환. data Buffer는 아직 생성하지 않음 |
| L3 source body 읽기 | `B/.../message.ts:133,309,327` →N `messageFrameData`, `N/addon_core.cc:1113` | `N/addon_message_values.h:125` native storage를 유지하는 external Buffer view →JS. payload copy 0. 이후 frame pack은 JS `.set` copy |
| source send 제출 | `B/runtime/buffers/message_conversion.ts:23,39` native handle/Buffer 인자 정규화→`B/runtime/messaging/completion_owner.ts:274` →N `socketSubmitSend`, `N/addon_core.cc:2000` | Buffer part마다 `N/addon_core.cc:904,211` JS→native copy; native Message는 `:924` reference copy, payload copy 0. 성공 cached result 또는 result/errno/completionId 객체 →JS 반환 |
| source request 제출 | `B/.../completion_owner.ts:379,398` →N `socketSubmitRequest`, `N/addon_core.cc:2051,2088` | 위와 같은 part 처리. result/errno/completionId 객체 →JS. `CompletionEntry`의 Promise/Map은 JS 작업이며 N-API 경계 아님 |
| target recv | `B/runtime/sockets/socket_operations.ts:222,231` →N `routerRecvMessageNoWait` (`N/addon_core.cc:3294`), `N:564,509` 결과 생성 | native mode: part snapshot·external handle·routingId·replyToken·routeGeneration →JS 반환. managed mode: part당 →JS allocator `Buffer.allocUnsafe(size)` callback 1회, `N/addon_message_values.h:248,258,265` native→JS 전체 copy 1회; 배열·routingId·token·generation 반환. 빈 receive는 null 반환 |
| native 수신 part `.data()` | `B/.../message.ts:309,321` →N `messageFrameCopyData` (`N/addon_core.cc:1129`) 이어 →N `messageFrameClose` (`:1275`) | `N/addon_message_values.h:234` napi_create_buffer_copy로 payload 전체 1회 native→JS copy, Buffer →JS 반환. Close는 undefined 반환. 읽지 않은 native part도 `Message.close():289`에서 close 1회 |
| target reply 제출 | `B/runtime/messaging/received_operations.ts:42,52`→`B/runtime/sockets/router_socket.ts:85,100,107` →N `socketReply`, `N/addon_core.cc:3145,3167` | Buffer만 copy, native Message는 shared ref. `zlink_reply` 이후 native handle 소비. undefined →JS 반환 |
| source completion poll | `B/runtime/messaging/completion_owner.ts:539,543` →N `socketCompletionRecv`, `N/addon_core.cc:2155,2174,2111` | 성공 reply part마다 →JS allocator callback 1회+payload copy 1회. completion 객체(3 bigint, status 숫자, rid Buffer, parts 배열) →JS 반환. `B/.../completion_owner.ts:181`과 `request_executor.ts`가 Buffer를 Message로 감쌈. 빈 poll null. source header 미관찰 여부와 무관하게 항상 두 part copy |
| poller wait | `B/runtime/eventing/`의 poller wait →N `pollerWaitInto`, `N/addon_core.cc:3817` | Core poll 이후 int32 →JS 반환. 내부 blocking wall time을 CPU 비용으로 세지 않음. frame payload copy 0 |

`B/runtime/messaging/message_materializer.ts:92`는 **앞선 record의 모든 part에서 managed data를 관찰했는지**로 다음 recv의 storage를 선택한다. L1은 header를 읽지 않으므로 native part별 `.data()/close` 경계가 남는다. L2/L3는 warmup 후 두 part 모두 managed로 받을 수 있어 per-part JS→native 경계가 줄고 allocator callback 2회가 된다. 이 차이는 audit 실제 호출 수로 검증한다.

`N/addon_core.cc:2330`의 readable watch는 등록한 callback을 `napi_make_callback`으로 호출할 수 있다. 현재 raw ladder가 이 경로를 쓰는지 export/callback 계수에 포함한다. JS callback 횟수와 N-API 내부 primitive 함수 호출 횟수는 다르다. Wrapper는 모든 addon export를 감싸지만 `napi_create_object` 같은 내부 C API 호출을 별도 경계로 과대 계수하지 않는다.

## 계측 범위

Preload는 `process.dlopen`이 반환한 **addon export 객체만** 감싼다. tracked binding/framework/bench 파일을 수정하지 않는다. Audit는 export별 호출·null/성공·예외·allocator callback·실제 Buffer 길이를 기록한다. CPU와 allocation profile은 export wrapper를 쓰지 않아 계수 overhead를 CPU 결과에 합치지 않는다. L3 native body가 pack 뒤 명시적으로 close되지 않는 source ladder의 경로도 profile에 그대로 남긴다(`T/client/main.js:267,277/316,318`: typed parts일 때만 close). Finalizer 비용을 codec self 비용으로 단정하지 않는다.

## boundary byte 회계

Payload copy와 routingId의 작은 stack copy를 분리한다. `N/addon_message_parts.h:17,33`은 source send/request마다 peer routingId 24B를 native stack에 복사한다. Target reply는 source routingId(`bench-source-request-<pid>`)를 같은 방식으로 복사한다. Source completion은 peer routingId 24B를 매 reply마다 JS Buffer로 복사한다(`N/addon_message_values.h:271`). Target recv는 warmup 뒤 같은 routingId Buffer를 재사용해 이 방향의 추가 routing copy가 없다. 숫자·bigint 인자 추출은 payload byte copy와 합산하지 않는다. Core 내부 TCP/framing·reference count 복사 역시 이 boundary bytes에 포함하지 않는다.

| 셀/역할, message당 payload B | L1 JS→native / native→JS | L2 JS→native / native→JS | L3 JS→native / native→JS |
|---|---:|---:|---:|
| send source | 1228 / 0 | 1247 / 0 | 2360 / 0 |
| send target | 0 / 1027 | 0 / 1247 | 0 / 1333 |
| request source (request 제출+reply 수신) | 1228 / 1228 | 1299 / 1240 | 2420 / 1336 |
| request target (request 수신+reply 제출) | 1228 / 1027 | 1240 / 1299 | 1336 / 1393 |

L1 고정 header 201B, L2 command/request/reply header 220/272/213B, protobuf body 1027B. L3 command/request/reply outer head 11/19/21B, multipart overhead 75B. L3 pack은 send 1247B, request 1299B, reply 1240B를 JS frame에 추가로 복사한다. 이 JS→JS/native-view write는 위 boundary copy와 별도다. L2 request/reply template의 JS copy 272/213B도 별도다. `messageFrameData`는 copy가 아니라 native Buffer view 생성이다.

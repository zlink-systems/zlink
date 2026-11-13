# Node L4 호출 형태 대조

L4는 L3의 service-wire 위에 기본 typed JSON payload encode/decode를 복원한다. channel envelope 모듈은 L2에서 이미 로드되므로 L4 전용 require-only 단계(L4R)는 추가하지 않았다.

| 경로 | binding raw 호출 | L4 ladder 호출 | 차이와 귀속 |
|---|---|---|---|
| Source send | `framework/bench/grpc/node/client/main.js:267-283,315-333`; protobuf `BenchPayload` body와 raw envelope를 제출 | `client/main.js:263-283,313-330`; production `encodeChannelEnvelopeParts()`로 JSON DTO를 인코딩하고 L3 service frame으로 제출 | L4: protobuf body 대신 typed JSON payload와 JSON content type. L2 header 및 L3 service wire는 선행 단계 |
| Target receive | `zlink-raw-server/main.js:64-92,95-119`; 재사용 Received, 같은 DontWait pump, payload protobuf decode | `zlink-framework-server/main.js:98-110,150-164,250-269`; L3 service frame 및 channel header decode 후 production `decodeChannelPayload()` | L4: 기본 JSON typed payload decode. receive loop, batch, drain, metrics는 binding과 동일 |
| Target reply | `zlink-raw-server/main.js:77-86`; protobuf reply body를 새 response envelope와 제출 | `zlink-framework-server/main.js:113-128`; `encodeChannelReplyParts()`로 typed JSON reply를 만들고 L3 service reply frame으로 제출 | L4: response channel header correlation과 JSON payload. reply는 새 part/frame이며 request parts 재사용 안 함 |
| Source completion / reply receive | `client/main.js:285-307`; reply protobuf body decode 및 parts close | `client/main.js:285-307`; L3 reply correlation 검증 후 production `decodeChannelReply()`와 JSON body decode | L4: typed JSON reply decode. completion pump와 parts close는 동일 |

## production 경로

- 송신: `framework/languages/node/packages/framework/src/runtime/channels/channel-outbound-operations.ts:79-90,213-224` → `channel-envelope.ts:471-532`의 기본 JSON payload encode.
- 수신: `framework/languages/node/packages/framework/src/runtime/channels/channel-dispatchers.ts:143-145,230-235` → `channel-dispatch-pipeline.ts:349-356` → `channel-envelope.ts:413-436`의 `decodeChannelPayload()`.
- Reply encode: `channel-dispatchers.ts:243` → `channel-envelope.ts:217-237`의 `encodeChannelReplyParts()`.
- Source reply decode: `channel-outbound-operations.ts:243-249` → `channel-envelope.ts:321-356`의 `decodeChannelReply()`.
- L4는 추가 wait, recv, drain, metrics atomic을 만들지 않는다. JSON bytes/base64 변환은 typed JSON codec에 속한다.

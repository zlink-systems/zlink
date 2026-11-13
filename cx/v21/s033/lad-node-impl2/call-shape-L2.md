# Node L2 호출 형태 대조

L2는 L1 permit 위에 production Framework channel envelope header를 요청과 응답 양쪽에 복원한다. 요청 header는 source에서 생성하고 target이 한 번 parse한다. 응답 header는 target reply 시 생성하고 source completion에서 한 번 parse한다. L2R은 encoder/decoder 모듈만 require하고 호출하지 않는다.

| 경로 | binding (`zlink-raw-server` / 공통 client) | L2 ladder | 차이와 귀속 |
|---|---|---|---|
| Source send | `client/main.js:249-258,276-286`; raw request/command envelope와 protobuf body를 2-part submit | `client/main.js:249-258,276-286`; `shared/raw-wire.js:83-96`의 `encodeChannelEnvelopeParts()` header와 protobuf body를 2-part submit | L2: production request header encode. protobuf request body 인코딩은 binding과 동일 |
| Target receive | `zlink-raw-server/main.js:64-92,95-119`; 재사용 Received, DontWait 256 batch, setImmediate pump | `zlink-framework-server/main.js:197-202`; 같은 L1 수신·pump와 permit, `:200`에서 `decodeChannelHeader()` 1회 | L1 permit 및 L2 request header parse. recv 횟수·drain·metrics는 binding과 동일 |
| Target reply | `zlink-raw-server/main.js:77-86`; 새 response envelope와 protobuf body를 2-part submit | `zlink-framework-server/main.js:197-212`; `shared/raw-wire.js:103-119`에서 production reply header와 새 protobuf body를 만들고 2-part submit | L2: `encodeChannelReplyParts()`가 request correlation을 이어 response header를 생성. 매 응답의 protobuf body를 새로 encode |
| Source completion / reply receive | `client/main.js:259-269`; completion 결과 처리, protobuf body decode, 모든 part close | `client/main.js:259-271`; `:262-265`에서 production response header를 1회 parse한 뒤 protobuf body decode와 part close | L2: response kind를 확인하는 production header parse 1회 추가 |

## Production 경로와 메시지당 차이

- Request encode: `channel-outbound-operations.ts:79-90,213-224` → `channel-envelope.ts:124-147,471-533,560-575`.
- Target request decode: `channel-dispatchers.ts:143-145` → `channel-envelope.ts:392-410`.
- Target reply encode: `channel-dispatchers.ts:409-412` → `channel-envelope.ts:217-237` (`encodeChannelReplyParts`).
- Source completion header parse: `channel-outbound-operations.ts:244-248,519-523`가 `decodeChannelReply()`로 completion을 처리한다. `channel-envelope.ts:321-330`은 `:325`에서 `decodeChannelHeader()`를 한 번 호출한다. L2 bench는 같은 production parser인 `decodeChannelHeader()`를 직접 한 번 호출한다.
- 추가 wait, atomics, recv 호출, drain은 없다. Target과 source에 각각 envelope header parse 1회가 추가된다. Reply protobuf body는 요청 part를 재사용하지 않고 매 응답 새로 encode한다.

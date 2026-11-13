# Node L2 호출 형태 대조

L2는 L1 permit 위에 production Framework channel envelope header encode/decode를 추가한다. L2R은 encoder/decoder 모듈만 require하고 호출하지 않는다.

| 경로 | binding (`zlink-raw-server` / 공통 client) | L2 ladder | 차이와 귀속 |
|---|---|---|---|
| Source send | `client/main.js:249-258,272-281`; raw envelope 상수와 protobuf body를 2-part submit | `client/main.js:249-258,272-281`; `shared/raw-wire.js:60-80`의 `encodeChannelEnvelopeParts()`가 만든 header와 기존 protobuf `Message`를 2-part submit | L2: production channel header encode. protobuf body 인코딩은 기존 raw 경로 그대로 |
| Target receive | `zlink-raw-server/main.js:64-92,95-119`; 재사용 Received, DontWait 256 batch, setImmediate pump | `zlink-framework-server/main.js:163-190,193-221,224-250`; 같은 L1 수신·pump와 permit, 추가로 `:196,216`에서 `decodeChannelHeader()` 1회 | L1 permit 및 L2 header parse. recv 횟수·drain·metrics는 binding과 동일 |
| Target reply | `zlink-raw-server/main.js:77-86`; 새 response envelope와 protobuf body를 2-part submit | `zlink-framework-server/main.js:199-205`; 새 `RESPONSE_ENVELOPE`와 protobuf body를 2-part submit | L2는 reply를 바꾸지 않음. L0 고정 handler 직접 호출 및 L1 permit 반환은 이전 단계 기능 |
| Source completion / reply receive | `client/main.js:259-269`; completion 결과 처리, 마지막 body decode, 모든 part close | `client/main.js:259-269`; 동일 | 동일. L2는 reply header parse를 추가하지 않음 |

## Production 경로와 메시지당 차이

- Encode: `channel-envelope.ts:124-147,471-533,560-575`; `encodeChannelEnvelopeParts()`가 production codec selection과 header serializer를 거친다. Bench codec은 미리 만들어 둔 빈 encoded payload를 반환해 기존 protobuf body를 재인코딩하거나 복사하지 않는다.
- Decode: `channel-envelope.ts:392-410`; `decodeChannelHeader()`가 `parts[0]`의 header를 검증한다. 수신 loop와 payload decode는 기존 L1 경로를 유지한다.
- 추가 wait, atomics, recv 호출, drain, body copy는 없다. Source는 L2 header 생성과 correlation/deadline 필드를 추가한다. Target은 header parse 한 번을 추가한다. Reply와 source completion은 그대로다.
- L2R server/client switch는 같은 production module load만 하고 encode/decode는 호출하지 않는다.

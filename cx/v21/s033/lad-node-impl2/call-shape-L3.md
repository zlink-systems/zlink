# Node L3 호출 형태 대조

L3는 L2 위에 M6A service header와 Framework multipart application frame을 추가한다. L3R은 L2 동작 위에서 service-wire codec만 require하고 호출하지 않는다.

| 경로 | binding (`zlink-raw-server` / 공통 client) | L3 ladder | 차이와 귀속 |
|---|---|---|---|
| Source send | `client/main.js:256-268,289-303`; L2 channel header와 protobuf body를 2-part submit | `client/main.js:256-268,289-303`; `shared/raw-wire.js:102-112`의 production `encodeChannelRequestHeader`/`encodeChannelSendHeader`와 `encodeMultipartApplicationPayload`로 L2 parts를 감싸 service header+frame 2-part submit | L3 service header, multipart frame encode, frame copy. L2 header/body는 그대로 안쪽에 포함 |
| Target receive | `zlink-raw-server/main.js:64-92,95-119`; 재사용 Received, DontWait 256 batch, setImmediate pump | `zlink-framework-server/main.js:94-107,126-165,168-202,231-266`; service header 및 frame decode 뒤 같은 L2 header/body 처리 | L3 service header 검증 및 multipart part view/materialization. recv·drain·metrics는 동일 |
| Target reply | `zlink-raw-server/main.js:77-86`; 새 response envelope와 protobuf body를 2-part submit | `zlink-framework-server/main.js:109-118`; `shared/raw-wire.js:115-125`에서 production `encodeReplyHeader`와 multipart application frame으로 감싸 outer 2-part submit | L3 reply header/correlation 및 frame encode. 안쪽 L2 response/header/body는 동일 |
| Source completion / reply receive | `client/main.js:269-286`; reply parts에서 protobuf body decode 후 close | `client/main.js:269-285`; service reply header/correlation 검증, multipart frame 및 inner parts decode 후 protobuf body decode | L3 reply header/correlation 및 frame decode; L2 response header는 파싱하지 않음 |

## Production 경로와 비용

- Channel service send/request header: `service-wire-m6a-codec.ts:93-115`; multipart application frame encode: `:216-229`; 생산 호출부는 `node-raw-mesh-backend.ts:2951-2958`이다.
- Server receive: service header `decodeHeader` (`service-wire-m6a-codec.ts:631-638`), application frame view 및 parts materialization (`:337-362`); 생산 호출부 `node-raw-mesh-backend.ts:2632-2639`이다. Reply header encode는 `raw-service-mesh-runtime.ts:981-985`; source reply validation은 `:1851-1868`이다.
- L3는 메시지마다 service correlation을 부여한다. L3 frame encode는 inner two parts를 한 application frame에 복사하고, receive는 frame view로 두 parts를 재구성한다. 추가 recv/wait/metrics atomic은 없다.
- 스위치: client `ZLINK_LADDER_L3_CLIENT` / `ZLINK_LADDER_L3R_CLIENT`; server `ZLINK_LADDER_L3_SERVICE_WIRE` / `ZLINK_LADDER_L3R_SERVICE_WIRE`. L3 스위치는 L2 header 경로도 포함한다.

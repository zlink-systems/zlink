# Node L4 writer profiling 호출 비교

ae8a5178b5 archive를 n4w-scratch에 풀고 그 안에서만 빌드한다. CPU와 할당 형태만 비교하며 처리량 판정을 하지 않는다. 모든 process는 CPU 6-7,14-15를 상속한다. measure.lock은 사용하지 않는다.

| 단계 | Binding bench (framework/bench/grpc/node/) | L3/L4 bench (같은 root, ae8a5178b5) | 같음 / 복원 기능 |
|---|---|---|---|
| source send | client/main.js:261-275,307-335; encodeBenchPayloadMessage | 같은 submitRequest/submitSend; L3 channel header + service-wire; L4 raw-wire.js:117-138 → production encodeChannelEnvelopeParts/encodeChannelReplyParts, codec 인자 없음 | socket과 submit 경로 같음; L2 envelope, L3 framing, L4 typed JSON |
| target receive | zlink-raw-server/main.js:47-53,64-81,95-112; reusable Received, DontWait, batch=256, setImmediate | zlink-framework-server/main.js:223-250,253-295; 같은 Received/DontWait/batch=256/setImmediate. L1 permit acquire → receive → releaseBeforeHandler; L3 unpack + channel decode; L4 raw-wire.js:141-149 → decodeChannelPayload | wait primitive 같음; L1 permit과 L2/L3/L4 해석만 복원 |
| target reply | zlink-raw-server/main.js:82-86; 새 protobuf body, 새 reply builder, 2 parts | zlink-framework-server/main.js:111-129; L3 production encodeChannelReplyParts와 protobuf codec, L4 codec 없는 기본 typed JSON. 매번 새 header/body, service-wire 2 parts | payload 재사용 없음; L2/L3/L4 encode만 복원 |
| source completion/reply receive | client/main.js:218-259,278-305; poller PollCompletion → wait(1ms), setImmediate, promise reply | 같은 completion pump; L3 decodeProductionServiceReply/header; L4 decodeProductionTypedReply → production decodeChannelReply → Base64 bytes | completion wait 같음; L2/L3/L4 decode만 복원 |

메시지별 회계: binding과 L3/L4 모두 JS atomics 없음, Received/poller 재사용. L1 permit의 비동기 acquire/release는 L3와 L4 양쪽에 동일하다. L3는 protobuf bytes, L4는 {body: Buffer}의 padded Base64 JSON이다. L4의 Buffer.from(parts[1], 'utf8')는 production이 반환한 string의 최초 UTF-8 byte 생성이다. raw-wire.js:146의 Buffer.from(encoded)는 수신 JSON 전체를 복사하는 bench 비용이며 production 원인으로 합산하지 않는다. 모든 응답은 새로 encode하며 request part를 재사용하지 않는다. 상세 production 복사·할당 회계는 report.md에 기록한다.


## 이번 후보

Baseline f723033a5a의 packages를 scratch에 복사하고 공식 build를 실행했다. Writer preload는 Framework JSON encode 함수만 같은 모듈 경계에서 바꾼다. payload·schema·bench·framing·socket·wait·counter는 바꾸지 않는다. Writer도 현재 toJSON invocation을 보존하므로 Buffer.toJSON Array(N)과 wrapper, byte input view 1개, Base64/JSON string은 양쪽에 남는다. JSON text 조립은 native JSON.stringify에서 writer의 순회·quote·문자열 결합으로 이동한다. 호출 비교 line은 이번 scratch에서 다시 확인했다.

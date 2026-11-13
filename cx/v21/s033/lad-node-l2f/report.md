# Node L2 reply envelope 리뷰 수정

## 경로 대조

| 작업 | production | bench |
|---|---|---|
| 요청 header decode | `framework/languages/node/packages/framework/src/runtime/channels/channel-dispatchers.ts:143-145` — `decodeChannelEnvelope` 한 번 | `framework/bench/grpc/node/zlink-framework-server/main.js:158-162` — `decodeProductionChannelHeader` 한 번. 요청별 중복 parse 없음 |
| 응답 encode | `channel-dispatchers.ts:409-412` — 재사용 codec registry로 `encodeChannelReplyParts`, 결과를 `record.reply`에 전달. protobuf serializer는 `framework/languages/node/packages/framework-codec-protobuf/src/framework.ts:81-88`에서 encoded payload 소유 | `framework/bench/grpc/node/shared/raw-wire.js:71-80,114-116` — 서버 초기화 때 만든 reply registry와 같은 envelope encoder 재사용. protobuf Buffer를 `ZLinkEncodedPayload.fromOwned`로 감싸고 반환 parts를 그대로 전달 |
| 응답 submit 타입 | `framework/languages/node/packages/framework/src/runtime/foundation/service-runtime-contracts.ts:296` — `reply`는 `MessageLike` 또는 배열 수용 | `framework/bench/grpc/node/zlink-framework-server/main.js:125-133` — reply builder에 두 part를 그대로 전달. 변환 불필요 |
| source completion parse | `framework/languages/node/packages/framework/src/runtime/channels/channel-outbound-operations.ts:243-248,518-523` — 각 완료 경로에서 `decodeChannelReply` 한 번 | `framework/bench/grpc/node/client/main.js:278-288` — L2 reply header 한 번 decode. body decode는 별도 protobuf parse |

## 수정 및 확인

- 응답 registry를 reply마다 만들지 않고 envelope 초기화 때 한 번 만들어 재사용.
- 불필요한 body 복사: 이전 2회(`Buffer.from(encoded.data())`, `Buffer.from(parts[1])`); 수정 후 추가 복사 0회. Serializer가 만든 protobuf Buffer를 owned payload로 감싸고 envelope codec의 `borrowEncodedPayload` 경로(`channel-envelope.ts:503-505`)로 그대로 전달.
- L3 service framing 호환을 위해 `framework/bench/grpc/node/zlink-framework-server/main.js:129`의 `close`는 존재할 때만 호출.
- 빌드: L2 `bash build.sh` 통과; tip `bash build.sh` 통과.
- 테스트: L2 bench `npm test` 20/20 통과; tip bench `npm test` 22/22 통과.
- 서버: tip L2 envelope 모드를 부하 없이 시작하고 SIGTERM 종료 성공.
- 측정·부하 실행 없음.

## 커밋 재작성

| 이전 | 새 커밋 |
|---|---|
| `696dab596a` L2 reply | `2bef18aa94` |
| `d7f9a45a25` L3 service wire | `20598f57a2` |
| `11442781b8` L4 typed codec | `ae8a5178b5` |
| `420196e7a1` L5-L7 | `7d1afa98a9` |

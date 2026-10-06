# StreamClient Protobuf tutorial

`.proto`에서 만든 `Ping`으로 보내고 push를 받으며, 요청 응답은 `Pong`으로 읽는다.
고정 타입 codec은 `Ping`에 사용하고, 응답은 `submitEncoded()`와 `fromProto(..., Pong)`으로 읽는다.
이 예제는 Node를 browser connector의 검증 환경으로 사용한다.

## 설치와 빌드

Node.js 22 이상과 npm을 사용한다. 이 디렉터리에서 실행한다.

```bash
npm ci
npm run build
```

빌드는 protobufjs static-module과 TypeScript 선언을 `generated/`에 만들고, 컴파일 결과와 함께
`dist/StreamClient/generated/`에 복사한다. 생성 파일은 Git에 넣지 않는다.

## 실행

서버 없이 확인하려면 다음 명령으로 검증용 WebSocket peer와 tutorial을 함께 실행한다.

```bash
npm run protobuf:check
# protobuf: push=hello, reply.rank=7
```

검증용 peer는 임시 로컬 포트에서 `Ping` send를 `Ping` push로 돌려주고, `Ping` request에는
`Pong { rank: 7 }`으로 응답한다. 잘못된 codec 번호와 손상된 bytes의 거부도 확인한다.

같은 protocol을 구현한 서버가 준비돼 있으면 주소를 환경 변수로 지정해 실행한다.

```powershell
$env:STREAM_PROTOBUF_ENDPOINT = 'ws://127.0.0.1:7721'
npm run protobuf
```

기존 JSON tutorial Server는 이 Protobuf protocol을 구현하지 않는다.
JSON 흐름을 실행하는 기존 `npm start`는 상위 tutorial README의 절차를 따른다.

## 가이드

- [Node Protobuf 송수신](../../../../doc/framework/node/guide/stream-connector/40-protobuf.ko.md)
- [Node Protobuf codec과 타입](../../../../doc/framework/node/guide/stream-connector/41-protobuf-codecs.ko.md)

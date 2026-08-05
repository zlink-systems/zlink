# 01 — 개요

[← 목차](INDEX.ko.md) | [다음: 브라우저 →](02-browser.ko.md)

---

TypeScript Stream Connector는 ZLink STREAM 서버에 연결하는 browser client library다. 브라우저 웹
client와 Unity WebGL, Cocos Creator web, Godot Web에서 같은 package root를 사용한다.

| 환경 | package | transport |
|------|---------|-----------|
| 브라우저 계열 | `@zlink-systems/stream-connector` | `ws`, `wss` |
| Node.js | connector 실행 비대상 | 서버와 browser runner만 실행 |

브라우저는 OS socket을 열 수 없으므로 `tcp://`와 `tls://` endpoint를 구성 오류로 즉시 거부한다.
WebSocket handshake, frame 처리와 TLS 인증서 검증은 플랫폼이 수행한다. connector에는 인증서 검증을
건너뛰는 option이 없다.

## package 책임

connector package는 lifecycle, request/reply, push, dispatch와 STREAM wire 연결을 담당한다. payload
codec은 필요한 package만 application이 선택해 connector 생성 option으로 넘긴다. MessagePack과
Protobuf package root는 browser-safe codec을 제공하고, Node framework 등록 기능은 각 package의
`./framework` subpath에 분리되어 있다. 따라서 browser bundle은 server framework runtime을 참조하지
않는다.

wire 계약의 정본은
[Stream Connector 공통 스펙](../../../common/spec/stream-connector/32-stream-connector.ko.md)이다.

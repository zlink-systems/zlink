# TypeScript Stream Connector

TypeScript STREAM client connector(`@zlink-systems/stream-connector`)의 문서 진입점이다. 대상은
브라우저 웹 client와 Unity WebGL, Cocos Creator web, Godot Web처럼 브라우저에서 실행되는 build다.
Node.js는 connector의 제품 실행 환경이 아니며 서버 process와 browser test runner만 담당한다.

| 문서 | 내용 |
|------|------|
| [가이드 INDEX](INDEX.ko.md) | 브라우저 연결, codec, dispatch와 flow 전달 |
| [03 — Unity WebGL](03-unity-webgl.ko.md) | `com.zlink.stream-connector.webgl` UPM 어댑터 |
| [TypeScript 공개 계약](../../../common/spec/stream-connector/languages/typescript/03-stream-connector.ko.md) | 정확한 public 타입과 package root |
| [Stream Connector 공통 스펙](../../../common/spec/stream-connector/32-stream-connector.ko.md) | 대상 환경, transport와 wire 계약 |

package root는 플랫폼 `WebSocket`으로 `ws`와 `wss` 연결을 제공한다. `/browser` subpath나 Node
socket 구현은 제공하지 않는다.

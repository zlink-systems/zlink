# TypeScript Stream Connector

> **⚠️ 이 가이드는 최신이 아니다.** 현재 리뷰·정비가 끝난 가이드는
> [`.NET` 가이드](../../../dotnet/README.ko.md) 하나뿐이다. 이 문서는 그 이전 상태이며,
> **`.NET` 가이드가 완성되면 이 문서를 삭제하고 그것을 기준으로 다시 쓴다.**
>
> **계약을 확인할 때는 이 문서를 믿지 말고 [spec 트리](../../../common/spec/README.ko.md)를 본다.**

TypeScript STREAM client connector(`@zlink-systems/stream-connector`)의 문서 진입점이다. 대상은
브라우저 웹 client와 Unity WebGL, Cocos Creator web, Godot Web처럼 브라우저에서 실행되는 build다.
Node.js는 connector 실행 환경이 아니며 test runner와 서버 process 실행만 담당한다.

| 문서 | 내용 |
|------|------|
| [가이드 INDEX](INDEX.ko.md) | 브라우저 연결, codec, dispatch와 flow 전달 |
| [TypeScript 공개 계약](../../../common/spec/stream-connector/languages/typescript/03-stream-connector.ko.md) | 정확한 public 타입과 package root |
| [Stream Connector 공통 스펙](../../../common/spec/stream-connector/32-stream-connector.ko.md) | 대상 환경, transport와 wire 계약 |

package root는 플랫폼 `WebSocket`으로 `ws`와 `wss` 연결을 제공한다. `/browser` subpath나 Node
socket 구현은 제공하지 않는다.

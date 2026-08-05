# .NET Stream Connector

`.NET` STREAM client connector(`Systems.Zlink.Stream.Connector`)의 문서 진입점이다.
데스크톱·서버 애플리케이션과 **네이티브 빌드 게임 엔진**(Unity, Godot C#)이 대상이다.

| 문서 | 내용 |
|------|------|
| [가이드 INDEX](INDEX.ko.md) | 개요, Unity, Godot C# |
| [.NET 공개 계약](../../../common/spec/stream-connector/languages/dotnet/03-stream-connector.ko.md) | public 타입과 시그니처 |
| [Stream Connector 공통 스펙](../../../common/spec/stream-connector/32-stream-connector.ko.md) | **정본** — 대상 환경, transport, wire 계약 |

> **웹(브라우저·WASM) 빌드에는 이 connector를 쓸 수 없다.** Unity WebGL과 Godot Web은
> [Node.js/TypeScript connector](../../../node/guide/stream-connector/README.ko.md)를 사용한다.

# TypeScript Stream Connector 가이드

TypeScript STREAM client connector(`@zlink-systems/stream-connector`)의 공식 사용자 가이드다.

| 문서 | 내용 |
|------|------|
| [01 — 개요](01-overview.ko.md) | 대상 실행 환경, package와 transport |
| [02 — 브라우저](02-browser.ko.md) | 연결, codec 주입, dispatch와 flow 전달 |

정확한 옵션, call builder와 오류 타입은
[TypeScript 공개 계약](../../../common/spec/stream-connector/languages/typescript/03-stream-connector.ko.md)이
소유한다.

## 다른 언어의 connector

| 문서 | 대상 |
|------|------|
| [C++ Stream Connector 가이드](../../../cpp/guide/stream-connector/INDEX.ko.md) | Unreal, Godot GDExtension, Axmol, 일반 C++ |
| [.NET Stream Connector 가이드](../../../dotnet/guide/stream-connector/INDEX.ko.md) | Unity 네이티브, Godot C#, desktop과 server |

웹 또는 WASM으로 빌드하면 언어와 무관하게 이 TypeScript connector를 사용한다.

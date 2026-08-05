# .NET Stream Connector 가이드

`.NET` STREAM client connector(`Systems.Zlink.Stream.Connector`)의 공식 사용자 가이드다.
데스크톱·서버 애플리케이션과 **네이티브 빌드 게임 엔진**(Unity, Godot C#)이 대상이다.

## 목차

| 문서 | 내용 |
|------|------|
| [01 — 개요](01-overview.ko.md) | 대상 실행 환경, 배포 단위, 엔진별 담당 connector |
| [02 — Unity (네이티브 빌드)](02-unity.ko.md) | `MonoBehaviour`에서 `Dispatch.Async()` 펌프, 일시 정지, 코루틴 프로젝트 |
| [03 — Godot C#](03-godot-csharp.ko.md) | `Node._Process`에서 펌프, signal 연동, 종료 처리 |

connector의 API 표면(옵션, send/request, codec, 오류)은
[.NET 공개 계약](../../../common/spec/stream-connector/languages/dotnet/03-stream-connector.ko.md)과
[.NET framework 가이드 09 — STREAM](../../../common/guide/server/09-stream.ko.md)이 다룬다.
이 가이드는 **엔진 통합**에 집중한다.

## 다른 언어의 connector

| 문서 | 대상 |
|------|------|
| [C++ Stream Connector 가이드](../../../cpp/guide/stream-connector/INDEX.ko.md) | Unreal, Godot(GDExtension), Axmol, 일반 C++ |
| [Node/TypeScript Stream Connector 가이드](../../../node/guide/stream-connector/INDEX.ko.md) | **브라우저, Unity WebGL, Cocos web**, Node |

**웹(브라우저·WASM)으로 빌드하면 언어와 무관하게 TypeScript connector를 사용한다.** 판단
기준은 [Stream Connector 공통 스펙 §2](../../../common/spec/stream-connector/32-stream-connector.ko.md)가 소유한다.

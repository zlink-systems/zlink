# 01 — 개요

[← 목차](INDEX.ko.md) | [다음: Unity →](02-unity.ko.md)

---

`.NET` Stream Connector는 ZLink STREAM 서버에 연결하는 client-side library다. 데스크톱·서버
애플리케이션과 **네이티브 빌드 게임 엔진**이 같은 STREAM 프로토콜을 사용한다.

## 대상 실행 환경

먼저 **어떤 환경으로 빌드하는지**를 정해야 한다. 환경의 제약이 사용할 connector를 결정한다.

| 대상 | connector | 이유 |
|------|-----------|------|
| 데스크톱·서버 애플리케이션 | **`.NET` connector** | OS 소켓을 그대로 사용한다 |
| **Unity — 네이티브 빌드**(PC·모바일·콘솔) | **`.NET` connector** | 같은 이유. main thread 제약만 추가된다([02](02-unity.ko.md)) |
| **Godot C# — 네이티브 빌드** | **`.NET` connector** | 같은 이유([03](03-godot-csharp.ko.md)) |
| **Unity — WebGL 빌드** | **TypeScript connector** | 브라우저 샌드박스는 OS 소켓을 열 수 없다 |
| **Godot — Web 빌드** | **TypeScript connector** | 같은 이유 |

**웹(브라우저·WASM)으로 빌드하는 순간 언어와 무관하게 TypeScript connector를 사용한다.**
브라우저 샌드박스에서 OS 소켓을 열 수 있는 언어가 없기 때문이다. 웹 빌드는
[Node/TypeScript connector 가이드](../../../node/guide/stream-connector/INDEX.ko.md)를 본다.

## 배포 단위

| 산출물 | 배포 형식 | 주요 사용자 |
|--------|-----------|-------------|
| `Systems.Zlink.Stream.Connector` | NuGet | 데스크톱·서버 애플리케이션, Unity(네이티브), Godot C# |

**Unity와 Godot C#은 전용 package를 두지 않는다.** 위 NuGet package를 그대로 사용한다.
엔진 통합에 필요한 것은 별도 API가 아니라 **dispatch 펌프 위치**뿐이다.

## 게임 엔진의 제약: main thread

엔진 객체(`GameObject`, `Node`, scene tree)는 main thread 밖에서 다루면 안 된다.
그래서 connector의 기본 dispatch mode는 **`Manual`** 이다. 이 모드에서는 사용자가
**main thread에서 명시적으로 펌프**할 때 handler와 event callback이 실행된다.

| 엔진 | 펌프 위치 |
|------|-----------|
| Unity | `MonoBehaviour.Update()`에서 `Dispatch.Async()` |
| Godot C# | `Node._Process(double)`에서 `Dispatch.Async()` |
| 일반 애플리케이션 | 펌프가 필요 없으면 `Dispatch.Immediate`로 바꾼다 |

**펌프하지 않으면 handler와 event는 실행되지 않는다.** `PendingDispatchCount`로 아직 처리하지
않은 callback 수를 확인한다.

## transport 지원

| scheme | transport |
|--------|-----------|
| `tcp://host:port` | TCP |
| `tls://host:port` | TLS over TCP |
| `ws://host:port/path` | WebSocket |
| `wss://host:port/path` | WebSocket over TLS |

네이티브 빌드에서는 네 가지를 모두 사용한다.

## 서버 framework와의 관계

connector는 STREAM 서버에 연결하는 client library다. 서버 framework package에 의존하지 않는다.

wire 계약과 연결 생명주기의 정본은
[Stream Connector 공통 스펙](../../../common/spec/stream-connector/32-stream-connector.ko.md),
`.NET` public 표면의 정본은
[.NET 공개 계약](../../../common/spec/stream-connector/languages/dotnet/03-stream-connector.ko.md)이다.

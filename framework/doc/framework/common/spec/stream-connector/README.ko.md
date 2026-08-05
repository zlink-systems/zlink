# Stream Connector 스펙

[스펙 목차](../README.ko.md)

Stream connector는 브라우저와 게임 엔진에서 실행되는 client package다. Framework
host와 배포 단위, 실행 환경과 의존성이 다르므로 별도 package로 배포한다. Wire와
session 동작은 Framework 공통 계약을 따른다.

| 문서 | 범위 |
|------|------|
| [32 Stream Connector](32-stream-connector.ko.md) | Framework가 보장하는 실행 환경, transport, wire, lifecycle과 배포 산출물을 정의한다. |

이 디렉토리는 server 쪽 public interface를 정의하지 않는다. Connector가 연결하는 server session은
[server/30 STREAM 서버 세션](../19-stream-session.ko.md)과
[server/31 Session Actor Dispatch](../20-session-actor-dispatch.ko.md)가 소유한다.

## 언어별 public API

| 언어 | 문서 |
|------|------|
| C++ | [03 Stream Connector](languages/cpp/03-stream-connector.ko.md)은 일반 C++, Unreal, Godot와 Cocos/Axmol의 정확한 public interface를 정의한다. |
| `.NET` | [03 Stream Connector](languages/dotnet/03-stream-connector.ko.md)은 .NET, Unity와 Godot의 정확한 public interface를 정의한다. |
| Java | [03 Stream Connector](languages/java/03-stream-connector.ko.md)는 Java의 정확한 public interface를 정의한다. |
| TypeScript | [languages/typescript](languages/typescript/README.ko.md)는 browser connector의 정확한 public interface를 정의한다. |

Browser connector와 Node.js Framework는 서로 다른 package다. Browser connector는 Node.js host의 public interface가 아니다
([00 §4](../00-public-contract-governance.ko.md)).

## 사용 안내

이 트리는 **계약**만 소유한다. 사용법은
[C++](../../../cpp/guide/stream-connector/README.ko.md),
[.NET](../../../dotnet/guide/stream-connector/README.ko.md),
[Java](../../../java/guide/stream-connector/README.ko.md),
[Kotlin](../../../kotlin/guide/stream-connector/README.ko.md),
[Node.js/TypeScript](../../../node/guide/stream-connector/README.ko.md)의 언어별 가이드에서
확인한다.

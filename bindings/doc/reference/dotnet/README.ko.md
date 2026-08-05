한국어 | [English](README.en.md)

[.NET 바인딩 스펙](../../spec/dotnet/README.ko.md) · [.NET 바인딩 가이드](../../guide/dotnet/index.ko.md)

# .NET bindings 레퍼런스

작성 규칙은 [레퍼런스 문서 작성 가이드](../../../../doc/principal/documentation/reference-writing-guide.ko.md)를
따른다. 이는 bindings 계층(Core C ABI의 언어별 투영)이다 — framework 계층(`Systems.Zlink.Framework`)이
아니다. Framework는 `framework/doc/framework/dotnet/reference/`에 이미 자신의 레퍼런스 트리가
있다.

Category는 [.NET 바인딩 스펙](../../spec/dotnet/README.ko.md)의 "Contract 폴더 레이아웃" 절이
정의하는 "Contract folder layout"(`Contracts/Core`, `Contracts/Messaging`, `Contracts/Sockets`,
`Contracts/Eventing`, `Contracts/Service`, `Contracts/Errors`)을 그대로 따른다 — 그 스펙은
다른 모든 wrapper binding(cpp/java/node/rust)이 자신의 contract category를 맞추는 parity
참조 lane이기도 하므로, 이 category 순서와 분할은 그 언어들의 레퍼런스 트리를 작성할 때도
변경 없이 이어진다.

## 로케일 관례

`bindings/doc/spec/<lang>/`의 모든 문서는 English 원본, Korean 번역이다(framework의
interface-catalog 관례와 반대). 이 레퍼런스 트리도 같은 방향을 따른다 — `.en.md`를 먼저,
`.ko.md`를 나중에 쓰고, 모든 spec 인용은 같은 로케일의 spec 파일을 가리킨다.

## Category

[.NET 바인딩 스펙](../../spec/dotnet/README.ko.md)은 `Contracts/Service/`(SPOT node, Spot,
Actor)를 포함한 6개 category를 정의한다. 그 category는 스펙이 binding이 "정렬됐다"고
간주하기 위한 필수 목표 형태로 기술하고 있어, 이 레퍼런스 트리가 임의로 목표 형태에서
빼버릴 수 있는 게 아니다. 하지만 현재 `bindings/dotnet/src/Zlink/Contracts/`에는
`Service/` 폴더가 없다 — SPOT/Actor는 framework 계층(`Systems.Zlink.Framework`)에만
다른 타입 이름으로 존재한다. 레퍼런스 tier는 caller가 실제로 호출할 수 있는 표면을
문서화한다는 원칙(core 레퍼런스를 열망적 스펙 문구가 아니라 `zlink.h` 기준으로 작성했던
것과 같은 원칙)에 따라, **`Contracts/Service/`가 소스에 존재하기 전까지 이 트리는
6개가 아니라 5개 category만 갖는다.** 이 공백이 미구현 목표인지 framework 계층으로
영구히 옮겨간 설계인지는 스펙 차원의 질문이며 이 문서의 범위 밖이다.

| Category | 상태 | Contract 원본(`Contracts/` 대조 확인) |
|---|---|---|
| [Core](01-core.ko.md) | 작성 완료 | `Contracts/Core/`: `Context.cs`, `ContextOptions.cs`, `RoutingId.cs`, `Zlink.cs`, `AtomicCounter.cs`, `ZlinkStopwatch.cs`, `ZlinkThread.cs` |
| [Messaging](02-messaging.ko.md) | 작성 완료 | `Contracts/Messaging/`: `Message.cs`, `MessageOperations.cs`, `OperationContracts.cs`, `Received.cs`, `SubscriptionEvent.cs`, `TopicMessage.cs`(`MessageEnvelopeParts.cs`는 `internal`이라 public 항목 없음) |
| [Sockets](03-sockets.ko.md) | 작성 완료 | `Contracts/Sockets/`: `ISocket.cs`, `IStreamSocket.cs`, `MessageSocketContracts.cs`, `RoutedSocketContracts.cs`, `PubSubSocketContracts.cs`, `SocketEnums.cs`, `SocketOptionFacades.cs`, `PubSubSocketOptionFacades.cs`, `RoutedSocketOptionFacades.cs` |
| [Eventing](04-eventing.ko.md) | 작성 완료 | `Contracts/Eventing/`: `EventEnums.cs`, `Monitor.cs`, `PollEvent.cs`, `Poller.cs`, `Timer.cs`, `ZlinkPoll.cs` |
| [Errors](05-errors.ko.md) | 작성 완료 | `Contracts/Errors/`: `Errors.cs`, `SubmitResult.cs`, `TypedExceptions.cs` |

"Core" 표기에 대해: 이는 문자 그대로 소스 폴더 이름 `Contracts/Core/`(context lifecycle,
context option, `RoutingId`, utility resource)를 가리키는 것이지, 이 저장소의 `core/` C
library가 아니다. 이름 충돌은 실재하며, 모든 wrapper 언어의 레퍼런스 트리가 이 이름을
쓰게 되면 모호하게 읽힐 수 있다 — 그렇다면 이는 이 트리가 일방적으로 바꿀 일이 아니라
bindings 스펙 소유자가 판단할 이름 문제다.

이 문서 트리는 `mkdocs.yml` nav에 올라가 있다.

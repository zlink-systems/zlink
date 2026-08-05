한국어 | [English](README.en.md)

[Java 바인딩 스펙](../../spec/java/README.ko.md) · [Java 바인딩 가이드](../../guide/java/index.ko.md)

# Java bindings 레퍼런스

작성 규칙은 [레퍼런스 문서 작성 가이드](../../../../doc/principal/documentation/reference-writing-guide.ko.md)를
따른다. 이는 bindings 계층(Core C ABI의 언어별 투영)이다 — framework 계층(자신의
레퍼런스 트리가 `framework/doc/framework/java/reference/`에 있음)이 아니다. Kotlin은 이
bindings 런타임을 그대로 공유한다(별도 `bindings/kotlin/` contract 소스가 없다 —
`bindings/kotlin/samples/`만 존재) — 그래서 이 트리가 Kotlin의 bindings 계층 레퍼런스도
겸한다.

Category는 [.NET 바인딩 스펙](../dotnet/README.ko.md)의 Contract Folder Layout을 공통
아키텍처 지도로 따른다. dotnet·cpp와 마찬가지로 이 트리도 6개가 아니라 5개
category다 — `systems.zlink.contracts`엔 `service` 패키지가 없다. SPOT/Actor는
framework 계층에만 존재한다. 아래 Contract 원본 열은 스펙 산문이 아니라 실제 패키지
목록과 대조 확인한 것이다.

아래 모든 category에 적용되는 Java 고유 참고 두 가지:

- **`contracts/` 아래 일부 파일은 public 패키지에 있어도 package-private이라
  public contract가 아니다**(`ContractAccess.RoutingIdAccess` 같은 내부 등록 hook,
  `Zlink.sleep(int)`/`Zlink.errno()`/`Zlink.multipartClose(Message[])`처럼 `public`
  수식어가 없는 특정 overload — `Zlink.sleep(Duration)`만 public이다). 아래 각 항목은
  실제로 도달 가능한 overload를 명시한다.
- **Option facade는 public 생성자로 만드는 concrete class다**(`new
  ContextOptions(context)`) — dotnet의 `IContext.Options`처럼 property/method로만
  얻는 게 아니다. 다만 `Context.options()`도 존재하며 이게 일반적인 경로다.

## 로케일 관례

`bindings/doc/spec/<lang>/`의 모든 문서는 English 원본, Korean 번역이다(framework의
interface-catalog 관례와 반대). 이 레퍼런스 트리도 같은 방향을 따른다 — `.en.md`를
먼저, `.ko.md`를 나중에 쓰고, 모든 spec 인용은 같은 로케일의 spec 파일을 가리킨다.

## Category

| Category | 상태 | Contract 원본(`systems.zlink.contracts/` 대조 확인) |
|---|---|---|
| [Core](01-core.ko.md) | 작성 완료 | `contracts/core/`: `Context.java`, `ContextOptions.java`, `ContextOption.java`, `RoutingId.java`, `Zlink.java`, `ZlinkVersion.java`, `AtomicCounter.java`, `ZlinkStopwatch.java`, `ZlinkThread.java` |
| [Messaging](02-messaging.ko.md) | 작성 완료 | `contracts/messaging/`: `Message.java`, `Received.java`, `TopicMessage.java`, `SubscriptionEvent.java`, `SubscriptionEntry.java`, `SendOperation.java`, `SendSubmitOperation.java`, `RequestOperation.java`, `RequestSubmitOperation.java`, `RequestCallbackSubmitOperation.java`, `TimeoutSubmitOperation.java`, `ReplyOperation.java`, `ReplySubmitOperation.java`, `MessageBuilderStage.java` |
| [Sockets](03-sockets.ko.md) | 작성 완료 | `contracts/sockets/`: `Socket.java`, `StreamSocket.java`, `MessageSocketContracts/{PairSocket,DealerSocket}.java`, `RoutedSocketContracts/RouterSocket.java`, `PubSubSocketContracts/{PubSocket,SubSocket,XPubSocket,XSubSocket}.java`, `SocketOptionFacades/*.java`, `SocketEnums/*.java`, `SocketHandlers/*.java` |
| [Eventing](04-eventing.ko.md) | 작성 완료 | `contracts/eventing/`: `Poller.java`, `ZlinkTimer.java`, `SocketMonitor.java`, `EventEnums/*.java`, `EventHandlers/*.java`, `EventModels/*.java` |
| [Errors](05-errors.ko.md) | 작성 완료 | `contracts/errors/`: `ErrorCategory.java`, `Errors/*.java`(exception class + result enum) |

이 문서 트리는 `mkdocs.yml` nav에 올라가 있다.

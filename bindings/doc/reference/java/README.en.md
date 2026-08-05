[한국어](README.ko.md) | English

[Java binding spec](../../spec/java/README.en.md) · [Java binding guide](../../guide/java/index.en.md)

# Java bindings reference

The writing rules follow the
[Reference-writing guide](../../../../doc/principal/documentation/reference-writing-guide.ko.md)
(Korean-only). This is the bindings layer (the Core C ABI's language projection) — not the
framework layer, which has its own reference tree under `framework/doc/framework/java/reference/`.
Kotlin shares this same bindings runtime (no separate `bindings/kotlin/` contract source exists —
only `bindings/kotlin/samples/`), so this tree also serves as Kotlin's bindings-layer reference.

Categories follow the [.NET binding spec](../dotnet/README.en.md)'s Contract Folder Layout as the
common architecture map. As with dotnet and cpp, this tree has five categories, not six —
`systems.zlink.contracts` has no `service` package; SPOT/Actor exists only at the framework layer.
The Contract-source column below is verified against the actual package listing, not copied from
spec prose.

Two Java-specific notes carried into every category below:

- **Several files under `contracts/` are package-private, not public contract**, even though they
  live in a public package (`ContractAccess.RoutingIdAccess`-style internal registration hooks,
  and specific overloads like `Zlink.sleep(int)`/`Zlink.errno()`/`Zlink.multipartClose(Message[])`
  that have no `public` modifier — only `Zlink.sleep(Duration)` is public). Each entry below states
  explicitly which overloads are actually reachable.
- **Options facades are concrete classes constructed with a public constructor** (`new
  ContextOptions(context)`), not obtained only through a property/method the way dotnet's
  `IContext.Options` works — though `Context.options()` also exists and is the normal path.

## Locale convention

Every `bindings/doc/spec/<lang>/` document is English-original, Korean-translation (unlike the
framework's interface-catalog convention). This reference tree follows the same direction: write
`.en.md` first, `.ko.md` second, and every spec citation links to the same-locale spec file.

## Category

| Category | Status | Contract source (verified against `systems.zlink.contracts/`) |
|---|---|---|
| [Core](01-core.en.md) | Drafted | `contracts/core/`: `Context.java`, `ContextOptions.java`, `ContextOption.java`, `RoutingId.java`, `Zlink.java`, `ZlinkVersion.java`, `AtomicCounter.java`, `ZlinkStopwatch.java`, `ZlinkThread.java` |
| [Messaging](02-messaging.en.md) | Drafted | `contracts/messaging/`: `Message.java`, `Received.java`, `TopicMessage.java`, `SubscriptionEvent.java`, `SubscriptionEntry.java`, `SendOperation.java`, `SendSubmitOperation.java`, `RequestOperation.java`, `RequestSubmitOperation.java`, `RequestCallbackSubmitOperation.java`, `TimeoutSubmitOperation.java`, `ReplyOperation.java`, `ReplySubmitOperation.java`, `MessageBuilderStage.java` |
| [Sockets](03-sockets.en.md) | Drafted | `contracts/sockets/`: `Socket.java`, `StreamSocket.java`, `MessageSocketContracts/{PairSocket,DealerSocket}.java`, `RoutedSocketContracts/RouterSocket.java`, `PubSubSocketContracts/{PubSocket,SubSocket,XPubSocket,XSubSocket}.java`, `SocketOptionFacades/*.java`, `SocketEnums/*.java`, `SocketHandlers/*.java` |
| [Eventing](04-eventing.en.md) | Drafted | `contracts/eventing/`: `Poller.java`, `ZlinkTimer.java`, `SocketMonitor.java`, `EventEnums/*.java`, `EventHandlers/*.java`, `EventModels/*.java` |
| [Errors](05-errors.en.md) | Drafted | `contracts/errors/`: `ErrorCategory.java`, `Errors/*.java` (exception classes + result enums) |

This document tree is wired into `mkdocs.yml` nav.

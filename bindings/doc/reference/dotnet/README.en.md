[한국어](README.ko.md) | English

[.NET binding spec](../../spec/dotnet/README.en.md) · [.NET binding guide](../../guide/dotnet/index.en.md)

# .NET bindings reference

The writing rules follow the
[Reference-writing guide](../../../../doc/principal/documentation/reference-writing-guide.ko.md)
(Korean-only). This is the bindings layer (the Core C ABI's language projection) — not the
framework layer (`Systems.Zlink.Framework`), which already has its own reference tree under
`framework/doc/framework/dotnet/reference/`.

Categories follow the "Contract folder layout" the
[.NET binding spec](../../spec/dotnet/README.en.md#contract-folder-layout) defines
(`Contracts/Core`, `Contracts/Messaging`, `Contracts/Sockets`, `Contracts/Eventing`,
`Contracts/Service`, `Contracts/Errors`) verbatim — that spec is also the parity-reference lane
every other wrapper binding (cpp/java/node/rust) aligns its own contract categories to, so this
category order and split carries over unchanged when those languages' reference trees are
written.

## Locale convention

Every `bindings/doc/spec/<lang>/` document is English-original, Korean-translation (unlike the
framework's interface-catalog convention). This reference tree follows the same direction: write
`.en.md` first, `.ko.md` second, and every spec citation links to the same-locale spec file.

## Category

The [.NET binding spec](../../spec/dotnet/README.en.md#contract-folder-layout) names six
categories, including `Contracts/Service/` (SPOT node, Spot, Actor). That category is a blueprint
target the spec describes as required for the binding to be "aligned" — it is not something this
reference tree can casually drop from the target shape. But `bindings/dotnet/src/Zlink/Contracts/`
has no `Service/` folder today; SPOT/Actor exists only at the framework layer
(`Systems.Zlink.Framework`), under different type names. Since a reference tier documents the
surface a caller can actually reach — the same rule this tree already applied when core's
reference was built from `zlink.h` rather than from aspirational spec text — **this tree has five
categories, not six, until `Contracts/Service/` exists in source.** Whether that gap is an
unimplemented target or a design that moved permanently to the framework layer is a spec-level
question outside this document's scope.

| Category | Status | Contract source (verified against `Contracts/`) |
|---|---|---|
| [Core](01-core.en.md) | Drafted | `Contracts/Core/`: `Context.cs`, `ContextOptions.cs`, `RoutingId.cs`, `Zlink.cs`, `AtomicCounter.cs`, `ZlinkStopwatch.cs`, `ZlinkThread.cs` |
| [Messaging](02-messaging.en.md) | Drafted | `Contracts/Messaging/`: `Message.cs`, `MessageOperations.cs`, `OperationContracts.cs`, `Received.cs`, `SubscriptionEvent.cs`, `TopicMessage.cs` (`MessageEnvelopeParts.cs` is `internal`, no public entry) |
| [Sockets](03-sockets.en.md) | Drafted | `Contracts/Sockets/`: `ISocket.cs`, `IStreamSocket.cs`, `MessageSocketContracts.cs`, `RoutedSocketContracts.cs`, `PubSubSocketContracts.cs`, `SocketEnums.cs`, `SocketOptionFacades.cs`, `PubSubSocketOptionFacades.cs`, `RoutedSocketOptionFacades.cs` |
| [Eventing](04-eventing.en.md) | Drafted | `Contracts/Eventing/`: `EventEnums.cs`, `Monitor.cs`, `PollEvent.cs`, `Poller.cs`, `Timer.cs`, `ZlinkPoll.cs` |
| [Errors](05-errors.en.md) | Drafted | `Contracts/Errors/`: `Errors.cs`, `SubmitResult.cs`, `TypedExceptions.cs` |

On "Core": this names the literal source folder `Contracts/Core/` (context lifecycle, context
options, `RoutingId`, utility resources) — it is not this repository's `core/` C library. The name
collision is real; if it reads as ambiguous once every wrapper language's reference tree uses it,
that is a naming call for whoever owns the bindings spec, not something this tree renames
unilaterally.

This document tree is wired into `mkdocs.yml` nav.

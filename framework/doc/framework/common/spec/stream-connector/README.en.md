# Stream Connector Spec

[Spec table of contents](../README.en.md)

Stream connector is a client package that runs in a browser and game
engine. Since it differs from the Framework host in deployment unit,
execution environment, and dependency, it's distributed as a separate
package. Wire and session behavior follows the Framework common
contract.

| Document | Scope |
|------|------|
| [32 Stream Connector](32-stream-connector.en.md) | Defines the execution environment, transport, wire, lifecycle, and deployment deliverable the Framework guarantees. |

This directory doesn't define the server-side public interface. The
server session a connector connects to is owned by
[server/30 STREAM Server Session](../19-stream-session.en.md) and
[server/31 Session Actor Dispatch](../20-session-actor-dispatch.en.md).

## Per-Language Public API

| Language | Document |
|------|------|
| C++ | [03 Stream Connector](languages/cpp/03-stream-connector.en.md) defines the exact public interface of plain C++, Unreal, Godot, and Cocos/Axmol. |
| `.NET` | [03 Stream Connector](languages/dotnet/03-stream-connector.en.md) defines the exact public interface of .NET, Unity, and Godot. |
| Java | [03 Stream Connector](languages/java/03-stream-connector.en.md) defines Java's exact public interface. |
| TypeScript | [languages/typescript](languages/typescript/README.en.md) defines the browser connector's exact public interface. |

The browser connector and Node.js Framework are different packages.
The browser connector isn't a public interface of the Node.js host
([00 §4](../00-public-contract-governance.en.md)).

## Usage Guide

This tree only owns the **contract**. For usage, check the
per-language guide:
[C++](../../../cpp/guide/stream-connector/README.en.md),
[.NET](../../../dotnet/guide/stream-connector/README.en.md),
[Java](../../../java/guide/stream-connector/README.en.md),
[Kotlin](../../../kotlin/guide/stream-connector/README.en.md),
[Node.js/TypeScript](../../../node/guide/stream-connector/README.en.md).

---
title: "Stream Connector Overview · Node/TypeScript"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/stream-connector/01-overview.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Stream Connector Overview

<!-- framework-adapter-nav:start -->
[Guide Home](README.en.md) | [Next: Installation and the First Connection](02-getting-started.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/stream-connector/01-overview.en.md) · [C#/.NET](../../../dotnet/guide/stream-connector/01-overview.en.md) · [Java](../../../java/guide/stream-connector/01-overview.en.md) · [Kotlin](../../../kotlin/guide/stream-connector/01-overview.en.md) · **Node/TypeScript**
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "After reading this chapter"

    You can decide which connector a client outside the mesh uses to reach a STREAM server,
    and you know where the connector's responsibility ends.

A STREAM server treats one connection as a session. Unlike mesh calls between nodes, which name
their target, STREAM addresses **the connection itself**, so the server can also send first.
The Stream Connector is the client-side library that opens that connection, and it carries the
same packets the server session handles.

This chapter covers the connector's scope, the choice per runtime, and the published artifacts.
Installation and the first connection are covered by
[Installation and the First Connection](02-getting-started.en.md).

## 1. What the Connector Covers

A packet is the **unit of transfer that carries a payload behind a header holding a name and
metadata**. The connector builds and reads those packets, keeps the connection alive, and
restores it after a drop. What travels on top — chat, combat, orders — is defined by the
application. The connector has no domain of its own.

Application code never builds or reads header bytes. The public surface has no place for an
arbitrary header; it deals in names, metadata, and payloads.

## 2. The Boundary with the Server Framework

The connector package does not depend on the server framework package, and the server framework
package does not reference the connector either. The two share a single wire contract, so a
client build never downloads the server runtime.

The connector depends only on what a client needs to run: transport, codec, and compression.
That keeps the same protocol available in a web browser or a game engine, where a server runtime
cannot be hosted.

## 3. Choosing a Connector per Runtime

Which connector applies is decided by **the engine and the build target, not by the language**.
The same Unity project uses different connectors for its native build and its web build.

| Target | Native build | Web build (browser · WASM) |
|---|---|---|
| Unity | `.NET` connector | TypeScript connector — C# calls the JS layer through jslib interop |
| Godot | C++ connector (GDExtension) or `.NET` connector (Godot C#) | TypeScript connector |
| Cocos | C++ connector (Axmol adapter) | TypeScript connector (Cocos Creator web) |
| Unreal | C++ connector (plugin) | not applicable |
| Browser web client | — | TypeScript connector |
| Desktop and server applications | `.NET` · Java · C++ connector | — |

**A web build uses the TypeScript connector regardless of language.** No language can open an OS
socket inside the browser sandbox.

A game engine cannot touch engine objects outside the main thread. That is why the setting that
decides when receive callbacks run defaults to **pumping them yourself**, and why the C++
connector core builds in a configuration with exceptions and coroutines turned off.

## 4. Endpoints and Transports

The endpoint scheme decides the transport. With nothing else configured, this mapping applies.

| Scheme | Transport |
|---|---|
| `tcp://` | TCP |
| `tls://` | TLS over TCP |
| `ws://` | WebSocket |
| `wss://` | WebSocket over TLS |

Browser runtimes — web, Cocos web, Unity WebGL, Godot Web — can use `ws` and `wss` only. Giving
one of them a `tcp://` or `tls://` endpoint fails immediately as a configuration error instead of
failing quietly at connect time. This is a platform limit, not an implementation limit. Native
builds use every transport in the table.

## 5. The Unit of Transfer

Each packet has a **kind**, which says whether the packet expects an answer or is itself an answer.

| Kind | Meaning |
|---|---|
| Send | one-way packet that expects no answer |
| Request | packet that waits for an answer |
| Response | successful answer to a request |
| Error | failed answer to a request, or a stream error unrelated to any request |

An answer is matched to its request by the sequence the runtime assigns, so several requests may
be in flight and each completes on its own regardless of arrival order. Answers carry no packet
name.

The codec that turns a payload into bytes is chosen once when the connector is created, and the
default is JSON. The send and receive payload limits are 64KB each and are adjustable through
options. Metadata is the place for small values such as a trace id or a locale, and the whole of
it cannot exceed 1024 bytes.

## 6. Published Artifacts

| Target | Artifact | Channel |
|---|---|---|
| General C++ client | `zlink-stream-connector` | CMake · vcpkg · Conan |
| Unreal | `zlink-unreal-stream-connector` | source plugin |
| Godot (C++) | `zlink-godot-stream-connector` | source GDExtension |
| Cocos/Axmol | `zlink-axmol-connector` | source package |
| `.NET` desktop and server, Unity native, Godot C# | `Zlink.Stream.Connector` | NuGet |
| Java · Kotlin | `systems.zlink:zlink-stream-connector` | Maven |
| Browser runtimes | `@zlink-systems/stream-connector` | npm |
| Unity WebGL adapter | `com.zlink.stream-connector.webgl` | UPM source package |

Web targets share a single npm package because browser, Cocos web, Unity WebGL, and Godot Web are
all browser runtimes. Native engine adapters ship as source, following the convention that engine
build systems take adapters in as source. Unity native and Godot C# have no package of their own
and use the `.NET` connector as it is.

## 7. Next Chapters

- Install and exchange the first packet — [Installation and the First Connection](02-getting-started.en.md)
- Defaults and when they are validated — [Connector Options](03-connector-options.en.md)
- Connection state and reconnection — [Connection Lifecycle](06-lifecycle.en.md)

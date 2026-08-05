# 01 — Overview

[← Table Of Contents](INDEX.en.md) | [Next: Browser →](02-browser.en.md)

---

The TypeScript Stream Connector is a browser client library that connects to a ZLink STREAM server.
Browser web clients and Unity WebGL, Cocos Creator web, and Godot Web use the same package root.

| Environment | Package | Transport |
|------|---------|-----------|
| Browser family | `@zlink-systems/stream-connector` | `ws`, `wss` |
| Node.js | not a connector execution target | runs only the server and the browser runner |

Since a browser can't open an OS socket, `tcp://` and `tls://` endpoints are immediately rejected as
a configuration error. The WebSocket handshake, frame handling, and TLS certificate verification are
performed by the platform. The connector has no option to skip certificate verification.

## Package Responsibilities

The connector package handles lifecycle, request/reply, push, dispatch, and the STREAM wire
connection. The payload codec is chosen by the application, passing only the package it needs as a
connector creation option. The MessagePack and Protobuf package roots provide browser-safe codecs,
and the Node framework registration feature is separated into each package's `./framework` subpath.
So a browser bundle doesn't reference the server framework runtime.

The wire contract's authority is the
[Stream Connector Common Spec](../../../common/spec/stream-connector/32-stream-connector.en.md).

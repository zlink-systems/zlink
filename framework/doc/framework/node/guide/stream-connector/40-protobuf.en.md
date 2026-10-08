---
title: "Node Protobuf Messaging · Node/TypeScript"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/stream-connector/40-protobuf.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Node Protobuf Messaging

<!-- framework-adapter-nav:start -->
[Contents](README.en.md) | [Previous: Game Engine Integration](12-engine-integration.en.md) | [Next: Node Protobuf Codecs and Types](41-protobuf-codecs.en.md)
<!-- framework-adapter-nav:end -->

!!! info "After reading this chapter"

    You can generate TypeScript message classes from `.proto` and receive Protobuf pushes and replies.
    Code comes from the runnable `framework/languages/node/tutorial/StreamClient` example.

When a server uses Protobuf payloads, the client needs the same `.proto` definitions to read the bytes.
This example uses one codec to receive `Ping` and `Pong` pushes through `on(Type, handler)`.
Receiving 255 message kinds does not require 255 codecs. Pass each generated class to its handler registration.
[Protobuf Codecs and Types](41-protobuf-codecs.en.md) explains how types and names are selected.

## 1. Generating Message Code

A `.proto` file defines message field numbers and types. This example defines `Ping`, which carries a
string, and `Pong`, which carries a number.

```protobuf title="StreamClient/messages.proto"
--8<-- "framework/languages/node/tutorial/StreamClient/messages.proto:protobuf-schema"
```

Install the dependencies and run the generation command from `framework/languages/node/tutorial/StreamClient`.
The `protobufjs-cli` static-module target generates a constructor for each message that exists at runtime.

```bash
npm ci
npm run generate:protobuf
```

The command uses `pbjs -t static-module -w commonjs` to create `generated/messages.cjs` and
`pbts` to create `generated/messages.d.cts`. The `.cjs` extension loads the generated module as
CommonJS in this ESM project. These files are regenerated during the build.

## 2. Configuring the Codec

Import the generated messages and the browser entry point of `@zlink-systems/framework-codec-protobuf`.
The server serializer entry point, `./framework`, is not used in the client.

```typescript title="StreamClient/protobuf.ts"
--8<-- "framework/languages/node/tutorial/StreamClient/protobuf.ts:protobuf-imports"
```

Configure one codec when creating the connector. The configured `Ping` is the fallback when no receiving type is supplied.
Encoding uses the generated instance type; typed receiving uses the handler constructor. The `endpoint` is the WebSocket address of a server using Protobuf.

```typescript title="StreamClient/protobuf.ts"
--8<-- "framework/languages/node/tutorial/StreamClient/protobuf.ts:protobuf-register"
```

## 3. Receiving — Push Handlers

`on(Ping, handler)` supplies `Ping` as the decoding type; `on(Pong, handler)` supplies `Pong`.
The same codec invokes each constructor's `decode`, so handlers receive instances of different types.
The example also shows an explicit packet name and a name-only registration using the fallback type.

<iframe class="zlink-diagram" src="/common/diagrams/stream-protobuf-push-en.html" title="Protobuf push decoding" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/stream-protobuf-push-en.html" target="_blank">↗ Open larger</a></p>

```typescript title="StreamClient/protobuf.ts"
--8<-- "framework/languages/node/tutorial/StreamClient/protobuf.ts:typed-receive"
```

The server's packet name is `Ping`, so registrations taking the `Ping` constructor or an explicit name
receive the same push. Use the named form when the generated class name differs from the wire name.

## 4. Sending — Send and Request

Use a generated class instance to send the message received by the handlers above.
The connector derives the packet name from the constructor, and the codec calls that constructor's `encode`.

```typescript title="StreamClient/protobuf.ts"
--8<-- "framework/languages/node/tutorial/StreamClient/protobuf.ts:protobuf-send"
```

Receive a `Pong` reply with `submit(Pong)`. The request builder passes the generated class to
the existing codec's `decode(payload, Pong)` and returns a `Pong` instance.
Pass a cancellation signal with `submit(Pong, signal)`.

<iframe class="zlink-diagram" src="/common/diagrams/stream-protobuf-reply-en.html" title="Protobuf request and reply" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/stream-protobuf-reply-en.html" target="_blank">↗ Open larger</a></p>

```typescript title="StreamClient/protobuf.ts"
--8<-- "framework/languages/node/tutorial/StreamClient/protobuf.ts:protobuf-request"
```

For callback delivery, use `submitCallback(Pong, callback)`. In the default dispatch mode,
the callback runs when `dispatch()` is called. This tutorial uses `Immediate` mode.

```typescript title="StreamClient/protobuf.ts"
--8<-- "framework/languages/node/tutorial/StreamClient/protobuf.ts:protobuf-request-callback"
```

The type argument in `submit<Pong>()` is erased at runtime, so use `submit(Pong)` to pass the generated class.
The low-level alternative is to read the bytes from `submitEncoded()` with `fromProto(encodedReply, Pong)`.

## 5. Execution Result

This command starts the StreamClient verification WebSocket peer on an ephemeral local port and calls
the same tutorial function. A separate Server process is not needed. The peer sends `Ping` and `Pong { rank: 3 }` pushes and replies to requests with `Pong { rank: 7 }`.

```bash
cd framework/languages/node/tutorial/StreamClient
npm ci
# Before release: build the local connector and codec sources.
npm run prepare:local
npm run protobuf:check
# protobuf: Ping=hello, Pong.rank=3, reply.rank=7
```

Verification checks real WebSocket traffic, handler payloads for both message types, encoded bytes, and the
reply type. It also checks rejection of an incorrect codec number and malformed Protobuf bytes.
The verification program connects through Node's WebSocket and runs the connector's browser entry point.

## 6. Related Documents

- Type and name selection and fallback types — [Protobuf Codecs and Types](41-protobuf-codecs.en.md)
- Receiving type propagation and handler execution — [Receiving Packets](05-receiving.en.md)
- Send and request — [Sending Packets](04-sending.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>

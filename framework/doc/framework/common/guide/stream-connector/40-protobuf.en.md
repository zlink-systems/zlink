# Node Protobuf Messaging

!!! info "After reading this chapter"

    You can generate TypeScript message classes from `.proto` and receive Protobuf pushes and replies.
    Code comes from the runnable `framework/languages/node/tutorial/StreamClient` example.

When a server uses Protobuf payloads, the client needs the same `.proto` definitions to read the bytes.
This example receives a `Ping` push and a `Pong` reply. It uses a codec configured with one fixed type.
[Protobuf Codecs and Types](41-protobuf-codecs.en.md) explains the limits of automatic selection between receiving types.

## 1. Generating Message Code

A `.proto` file defines message field numbers and types. This example defines `Ping`, which carries a
string, and `Pong`, which carries a numeric reply.

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

Configure one codec when creating the connector. This codec treats both outgoing and incoming payloads
as `Ping`. The `endpoint` is the WebSocket address of a server using Protobuf.

```typescript title="StreamClient/protobuf.ts"
--8<-- "framework/languages/node/tutorial/StreamClient/protobuf.ts:protobuf-register"
```

## 3. Receiving — Push Handlers

The example registers three handler forms for the same `Ping` push. An application uses the one it needs.
Even the final handler, registered with only a name, receives a `Ping` with this fixed-type codec.

<iframe class="zlink-diagram" src="/common/diagrams/stream-protobuf-push-en.html" title="Protobuf push decoding" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/stream-protobuf-push-en.html" target="_blank">↗ Open larger</a></p>

```typescript title="StreamClient/protobuf.ts"
--8<-- "framework/languages/node/tutorial/StreamClient/protobuf.ts:typed-receive"
```

The server's packet name is `Ping`, so registrations taking the `Ping` constructor or an explicit name
receive the same push. Use the named form when the generated class name differs from the wire name.

## 4. Sending — Send and Request

Use a generated class instance to send the message received by the handlers above.
The connector derives the packet name `Ping` from its constructor.

```typescript title="StreamClient/protobuf.ts"
--8<-- "framework/languages/node/tutorial/StreamClient/protobuf.ts:protobuf-send"
```

A reply carrying `Pong` needs a separate decoding type. Call `submitEncoded()` to receive the reply
bytes, then pass the reply's generated class to `fromProto()`.

<iframe class="zlink-diagram" src="/common/diagrams/stream-protobuf-reply-en.html" title="Protobuf request and reply" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/stream-protobuf-reply-en.html" target="_blank">↗ Open larger</a></p>

```typescript title="StreamClient/protobuf.ts"
--8<-- "framework/languages/node/tutorial/StreamClient/protobuf.ts:protobuf-request"
```

The `<Pong>` in `submit<Pong>()` specifies only the TypeScript return type. The `Pong` constructor is
not passed to the codec at runtime, so the example specifies the reply type explicitly.

## 5. Execution Result

This command starts the StreamClient verification WebSocket peer on an ephemeral local port and calls
the same tutorial function. A separate Server process is not needed. The peer echoes `Ping` as a push
and replies to requests with `Pong { rank: 7 }`.

```bash
cd framework/languages/node/tutorial/StreamClient
npm ci
npm run protobuf:check
# protobuf: push=hello, reply.rank=7
```

Verification checks real WebSocket traffic, the payload received by all three registrations, and the
reply type. It also checks rejection of an incorrect codec number and malformed Protobuf bytes.
Node is used as a verification environment for this browser connector.

## 6. Related Documents

- Type and name selection and multiple-message limits — [Protobuf Codecs and Types](41-protobuf-codecs.en.md)
- Receiving type propagation and handler execution — [Receiving Packets](05-receiving.en.md)
- Send and request — [Sending Packets](04-sending.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>

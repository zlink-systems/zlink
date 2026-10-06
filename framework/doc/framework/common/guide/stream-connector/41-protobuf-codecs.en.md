# Node Protobuf Codecs and Types

!!! info "After reading this chapter"

    You can distinguish a generated-class codec from an envelope codec, and packet names from decoding types.
    Runnable code comes from the same StreamClient tutorial as [Protobuf Messaging](40-protobuf.en.md).

Protobuf bytes alone do not identify their message class. They contain field numbers and values,
but no class name such as `Ping` or `Pong`. Selecting a handler by packet name and choosing a class
to decode the bytes are separate operations.

## 1. Handler Types and Fallback Types

`createZlinkStreamProtobufCodec(Type)` calls the `decode` of the constructor registered with the handler.
`on(Ping, handler)` and `on(Pong, handler)` receive different types through the same codec.
A name-only registration uses the fallback `Type` supplied to the factory.
Encoding uses the generated instance's `encode`. Values whose constructor has no `encode` use the fallback type.

`createZlinkStreamProtobufEnvelopeCodec(options)` supports protocols that already use an envelope format.
An envelope is an outer message containing information that distinguishes message kinds.
The factory forwards the handler type to `options.decode(payload, messageType)`. The application's decoder
controls how the inner message is read. The factory does not automatically register message kinds.

| Surface                                           | Where the type is selected                   | Receiving behavior                                      |
| ------------------------------------------------- | -------------------------------------------- | ------------------------------------------------------- |
| `createZlinkStreamProtobufCodec(Type)`            | Handler registration, or factory fallback    | Calls the supplied constructor's `decode`               |
| `createZlinkStreamProtobufEnvelopeCodec(options)` | Handler registration and application decoder | Delegates to `options.decode(payload, messageType)`     |
| `fromProto(payload, ReplyType)`                   | At the call                                  | Reads an encoded payload using the supplied `ReplyType` |

!!! note "Fix Availability"

    Type forwarding above describes the fix in [#1503](https://github.com/zlink-systems/zlink/issues/1503).
    Both factories in released version 0.28.0 ignore handler types.

## 2. Server Packet Names and Generated Classes

A packet name is a separate name sent by the connector, not a field in the Protobuf schema.
For type-based sending and receiving, the default name resolver uses the constructor's `name`.
The example's `.proto` package is `tutorial`, but its packet name is `Ping`, not `tutorial.Ping`.

The names match when the server handler accepts `Ping` and uses `Ping` for pushes.
For a server name such as `player.ping`, specify it with the sending builder's
`packetName('player.ping')` and `on('player.ping', handler, Ping)`.
If bundling shortens constructor names, specify wire names explicitly or preserve constructor names.

## 3. Request and Reply Types

The constructor in `request(new Ping(...))` is used to encode the request and select its packet name.
It does not select the reply type. The type argument in `submit<Pong>()` also does not exist at runtime.
The current reply handling method is `submitEncoded()` followed by `fromProto(encodedReply, Pong)`.

<iframe class="zlink-diagram" src="/common/diagrams/stream-protobuf-reply-en.html" title="Explicit reply type decoding" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/stream-protobuf-reply-en.html" target="_blank">↗ Open larger</a></p>

```typescript title="StreamClient/protobuf.ts"
--8<-- "framework/languages/node/tutorial/StreamClient/protobuf.ts:protobuf-request"
```

Replies correlate with requests through their sequence, so the connector does not automatically find
a decoding type from the `Pong` packet name alone. The server and client need matching `.proto` field
numbers and types for replies too. Reading with the wrong schema does not always fail: unknown fields
may be ignored and default values returned.

## 4. Related Documents

- Code generation through execution verification — [Protobuf Messaging](40-protobuf.en.md)
- Handler type propagation — [Receiving Packets](05-receiving.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>

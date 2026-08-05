# Java STREAM Session Public Interface

[Interface table of contents](README.en.md) · [STREAM Session](../../../../19-stream-session.en.md)

STREAM session, Actor binding, and relay are owned by the JVM Framework
runtime. Only session lifecycle, typed packet/push, and the Actor
binding result are exposed to the application — transport frame and
binding implementation aren't exposed.

Session bind submits an exact `ActorRef` once. If there's no active
Message Follow route, `NOT_FOUND`; if the generation differs,
`INVALID_OPERATION`; if in pre-commit seal, `UNAVAILABLE`. The framework
doesn't find a new ref in the Store and hidden-retry.
`enableActorDispatch()` doesn't take a MeshName, and needs an Object
Client or Server role and a Location Store at startup.

If the Actor is on a different MeshNode, the runtime delivers bind,
ingress, and push through the same public session interface. The runtime
checks Actor ObjectGeneration, source/target NodeGeneration,
AuthorityOwnerGeneration, binding generation, and session sequence before
the application callback. Rebind and close are exact binding identity
transitions. Since identity uses the session owner Node RID/lifecycle
generation/owner-local
[binding generation](../../../../01-glossary.en.md#binding-generation)
together, even a small local counter of a different
[MeshNode](../../../../01-glossary.en.md#meshnode) or a restarted
[owner](../../../../01-glossary.en.md#owner) can be registered as a new
binding. A push/ingress/tombstone from a previous owner lifecycle isn't
applied to the current session.

Session close observes remote unbind completion within a bounded
lifecycle deadline. It doesn't ignore a timeout or terminal failure —
it returns it as a close failure, and cleans up local binding and
session transport regardless of success or failure. No additional
public member is provided for this behavior.

After bind, relay/request relay and `notifyDisconnected()` use the
per-Actor stored route and don't query the Location Store per message. A
physical disconnect has the framework perform an automatic all-settled
notification to every current binding, running the Spot callback at most
once per exact binding identity. `notifyDisconnected()` is a logical
notification while the connection is kept, and waits for the callback
terminal. A relocation route update is only allowed on the same
ObjectGeneration. After the target Actor is restored and starts message
processing, the target runtime sends `sessionActorLocationUpdateReqMsg`
and changes that Actor's route and the current `ActorRef` location
snapshot `ZLinkSessionActor.ref()` returns together. The snapshot
reflects the same ActorId/ObjectGeneration and the target MeshName/
NodeRid. Even without a response, target Actor processing doesn't stop,
and the same request is resent at a fixed interval. The route and
physical STREAM connection of a different Actor on the same Session that
isn't included in the relocation target are kept. The application
doesn't rebind to learn about relocation.

## STREAM Socket Message Size

`configureSocket().setMaxMessageSize(...)` applies to complete
client-to-server messages received by a StreamNode through Core STREAM. The
default is `64 KiB`, measured as header bytes plus payload bytes and excluding
the 6-byte prefix. `0` means no Framework limit and a negative value is a
startup error. An over-limit message isn't delivered to the handler; the
server records `EMSGSIZE` and closes the connection. The Framework limit
doesn't apply to server-to-client outbound messages.

## Exact Public Member Inventory

The declarations below fix this category's Java public types and
members.

```java
public interface systems.zlink.framework.streams.ZLinkSession {
  public abstract systems.zlink.framework.streams.ZLinkSessionContext context();
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> onConnected();
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> onDisconnected();
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> onError(systems.zlink.framework.streams.ZLinkStreamError);
  public default java.util.concurrent.CompletionStage<java.lang.Void> onDispatch(systems.zlink.framework.streams.ZLinkSessionDispatchContext, systems.zlink.framework.messaging.ZLinkMessage);
}
public interface systems.zlink.framework.streams.ZLinkSessionActor {
  public abstract java.lang.String actorId();
  public abstract systems.zlink.framework.actors.ActorRef ref();
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> relay(systems.zlink.framework.messaging.ZLinkMessage);
  public default java.util.concurrent.CompletionStage<java.lang.Void> relay(systems.zlink.framework.streams.ZLinkSessionDispatchContext, systems.zlink.framework.messaging.ZLinkMessage);
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> notifyDisconnected();
}
public interface systems.zlink.framework.streams.ZLinkSessionActors {
  public abstract java.util.List<systems.zlink.framework.streams.ZLinkSessionActor> bound();
  public abstract java.util.concurrent.CompletionStage<systems.zlink.framework.streams.ZLinkSessionActor> bind(systems.zlink.framework.actors.ActorRef);
  public abstract java.util.concurrent.CompletionStage<systems.zlink.framework.streams.ZLinkSessionActor> bindOrGet(systems.zlink.framework.actors.ActorRef);
  public abstract java.util.Optional<systems.zlink.framework.streams.ZLinkSessionActor> find(java.lang.String);
}
public interface systems.zlink.framework.streams.ZLinkSessionClient {
  public abstract systems.zlink.framework.streams.ZLinkSessionSendCall send(java.lang.Object);
  public abstract systems.zlink.framework.streams.ZLinkSessionReplyCall reply(java.lang.Object);
}
public interface systems.zlink.framework.streams.ZLinkSessionContext {
  public abstract java.lang.String sessionId();
  public abstract java.util.Optional<systems.zlink.contracts.core.RoutingId> routingId();
  public abstract java.util.Optional<java.lang.String> localAddr();
  public abstract java.util.Optional<java.lang.String> remoteAddr();
  public abstract systems.zlink.framework.streams.ZLinkSessionClient client();
  public abstract systems.zlink.framework.streams.ZLinkSessionActors actors();
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> close();
}
public interface systems.zlink.framework.streams.ZLinkSessionPacketDispatcher<TSessionContext extends systems.zlink.framework.streams.ZLinkSessionContext> {
  public abstract java.util.concurrent.CompletionStage<java.lang.Boolean> tryHandle(TSessionContext, systems.zlink.framework.streams.ZLinkSessionDispatchContext, systems.zlink.framework.messaging.ZLinkMessage);
}
public interface systems.zlink.framework.streams.ZLinkSessionReplyCall {
  public abstract systems.zlink.framework.streams.ZLinkSessionReplyCall compress();
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> submit();
}
public interface systems.zlink.framework.streams.ZLinkSessionSendCall {
  public abstract systems.zlink.framework.streams.ZLinkSessionSendCall metadata(java.lang.String, java.lang.String);
  public abstract systems.zlink.framework.streams.ZLinkSessionSendCall compress();
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> submit();
}
public interface systems.zlink.framework.streams.ZLinkStreamCompressionCodec {
  public abstract byte[] compress(byte[]);
  public abstract byte[] decompress(byte[], int);
}
public final class systems.zlink.framework.streams.ZLinkStreamError extends java.lang.Record {
  public systems.zlink.framework.streams.ZLinkStreamError(systems.zlink.framework.streams.ZLinkStreamSessionError, java.lang.String);
  public final java.lang.String toString();
  public final int hashCode();
  public final boolean equals(java.lang.Object);
  public systems.zlink.framework.streams.ZLinkStreamSessionError error();
  public java.lang.String message();
}
public final class systems.zlink.framework.streams.ZLinkStreamSessionError extends java.lang.Enum<systems.zlink.framework.streams.ZLinkStreamSessionError> {
  public static final systems.zlink.framework.streams.ZLinkStreamSessionError INTERNAL;
  public static final systems.zlink.framework.streams.ZLinkStreamSessionError TRANSPORT_ERROR;
  public static systems.zlink.framework.streams.ZLinkStreamSessionError[] values();
  public static systems.zlink.framework.streams.ZLinkStreamSessionError valueOf(java.lang.String);
  public int value();
}
public interface systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler<TSessionContext extends systems.zlink.framework.streams.ZLinkSessionContext, TMessage> {
  public abstract java.lang.Class<TMessage> messageType();
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> handle(TSessionContext, systems.zlink.framework.streams.ZLinkSessionDispatchContext, TMessage);
}
```

`relay(...)` taking only payload is one-way admission. The overload
taking a dispatch context immediately transfers the explicit current
STREAM request reply capability to the runtime at call time. If
submitted, the Actor typed reply completes the original STREAM
correlation terminal-once, and on admission failure the framework
completes the same correlation as a typed failure. The caller doesn't
perform a separate reply/retry. The one-way dispatch context has no
reply capability, so it only returns admission. A handshake failure is
only recorded in runtime monitoring before session creation, and isn't
delivered to `onError(...)`.

## STREAM Codec Public Signature

```java
public final class systems.zlink.framework.streams.ZLinkSessionDispatchContext extends java.lang.Record {
  public systems.zlink.framework.streams.ZLinkSessionDispatchContext(java.lang.String, java.util.Map<java.lang.String, java.lang.String>, boolean);
  public final java.lang.String toString();
  public final int hashCode();
  public final boolean equals(java.lang.Object);
  public java.lang.String packetName();
  public java.util.Map<java.lang.String, java.lang.String> metadata();
  public boolean canReply();
}
public final class systems.zlink.framework.streams.ZLinkStreamCodec extends java.lang.Enum<systems.zlink.framework.streams.ZLinkStreamCodec> {
  public static final systems.zlink.framework.streams.ZLinkStreamCodec RAW;
  public static final systems.zlink.framework.streams.ZLinkStreamCodec JSON;
  public static final systems.zlink.framework.streams.ZLinkStreamCodec MESSAGE_PACK;
  public static final systems.zlink.framework.streams.ZLinkStreamCodec PROTOBUF;
  public static systems.zlink.framework.streams.ZLinkStreamCodec[] values();
  public static systems.zlink.framework.streams.ZLinkStreamCodec valueOf(java.lang.String);
  public int value();
  public static systems.zlink.framework.streams.ZLinkStreamCodec fromValue(int);
}
public final class systems.zlink.framework.streams.ZLinkStreamCompressionCodecs {
  public static systems.zlink.framework.streams.ZLinkStreamCompressionCodec lz4();
}
public final class systems.zlink.framework.streams.ZLinkStreamMessageKind extends java.lang.Enum<systems.zlink.framework.streams.ZLinkStreamMessageKind> {
  public static final systems.zlink.framework.streams.ZLinkStreamMessageKind SEND;
  public static final systems.zlink.framework.streams.ZLinkStreamMessageKind REQUEST;
  public static final systems.zlink.framework.streams.ZLinkStreamMessageKind RESPONSE;
  public static final systems.zlink.framework.streams.ZLinkStreamMessageKind ERROR;
  public static final systems.zlink.framework.streams.ZLinkStreamMessageKind CONTROL;
  public static systems.zlink.framework.streams.ZLinkStreamMessageKind[] values();
  public static systems.zlink.framework.streams.ZLinkStreamMessageKind valueOf(java.lang.String);
  public int value();
  public static systems.zlink.framework.streams.ZLinkStreamMessageKind fromValue(int);
}
```

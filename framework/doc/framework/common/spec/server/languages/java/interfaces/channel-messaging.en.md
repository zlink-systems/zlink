# Java Channel Messaging Public Interface

[Interface table of contents](README.en.md) · [Channel Messaging](../../../../08-channel-messaging.en.md)

ChannelName selects the send path within a process. The exact payload
type of the RouteMesh/ClientServer/fanout builder, typed handler, and
client call projects the common contract's role distinction into Java
generics. A one-way operation only provides `submit()`, and a request
operation's `submit(...)` waits until the terminal reply. Logical
Multicast doesn't return per-target submit results or aggregate them into
publish-only monitoring. Remote Spot queue submission and remote/local
handler execution or completion aren't `CompletionStage` completion
conditions.

A filter is applied to RouteMesh/ClientServer Channel send/request, Node
direct send/request, and classic fanout subscription handlers. It isn't
applied to Spot/Actor/Logical Multicast/STREAM handlers. A filter
distinguishes the five paths via
`ZLinkHandlerFilterContext.dispatchKind()`. The RouteMesh and Node
direct context provides the actual MeshName. The ClientServer and
classic fanout context's MeshName is empty.

A child scope is created per handler dispatch. The handler and filter are
each created once by the framework in this scope, and use the same
scoped dependency. Spring bean scope or application service registration
can't change this lifetime. Once dispatch finishes, the framework cleans
up the instances it created and the child scope.

The filter's `next.invoke()` can be called at most once. A second call is
rejected with `IllegalStateException` and doesn't re-run the handler. If
a request filter doesn't call `next`, it completes with
`ZLinkFrameworkException`'s `REJECTED`. Even if a filter returns an
arbitrary value, it isn't used as the business reply. Only the value the
handler `next` ran produces becomes the reply.

Classic fanout creates a separate scope and filter chain per matching
handler. A filter interruption or failure in one dispatch doesn't cancel
the execution of a different handler dispatch.

```java
public interface ZLinkMeshChannelBuilder {
    ZLinkMeshChannelClientBuilder client();
    ZLinkMeshChannelServerBuilder server();
}

public interface ZLinkMeshChannelClientBuilder {}

public interface ZLinkMeshChannelServerBuilder {
    ZLinkMeshChannelServerBuilder setWeight(int weight);
    ZLinkMeshChannelServerBuilder addHandlerGroup(String groupName);
    <THandler extends ZLinkSendHandler<TMessage>, TMessage>
    ZLinkMeshChannelServerBuilder addSendHandler(
        Class<THandler> handlerType, Class<TMessage> messageType);
    <THandler extends ZLinkRequestHandler<TRequest, TReply>, TRequest, TReply>
    ZLinkMeshChannelServerBuilder addRequestHandler(
        Class<THandler> handlerType,
        Class<TRequest> requestType,
        Class<TReply> replyType);
}

public interface ClientServerChannelBuilder {
    ZLinkClientServerChannelClientBuilder client();
    ZLinkClientServerChannelServerBuilder server();
}

public interface ZLinkClientServerChannelClientBuilder {
    ZLinkClientServerChannelClientBuilder connect(String endpoint);
}

public interface ZLinkClientServerChannelServerBuilder {
    ZLinkClientServerChannelServerBuilder listen();
    ZLinkClientServerChannelServerBuilder listen(int port);
    ZLinkClientServerChannelServerBuilder setBindHost(String host);
    ZLinkClientServerChannelServerBuilder setAdvertiseHost(String host);
    ZLinkClientServerChannelServerBuilder setWeight(int weight);
    ZLinkClientServerChannelServerBuilder addHandlerGroup(String groupName);
    <THandler extends ZLinkSendHandler<TMessage>, TMessage>
    ZLinkClientServerChannelServerBuilder addSendHandler(
        Class<THandler> handlerType, Class<TMessage> messageType);
    <THandler extends ZLinkRequestHandler<TRequest, TReply>, TRequest, TReply>
    ZLinkClientServerChannelServerBuilder addRequestHandler(
        Class<THandler> handlerType,
        Class<TRequest> requestType,
        Class<TReply> replyType);
}

public interface ZLinkClient {
    ZLinkSendCall sendToChannel(String channelName, Object message);
    ZLinkRequestCall requestToChannel(String channelName, Object request);
}

public interface ZLinkRouteClient {
    ZLinkSendCall sendToNode(
        String meshName, RoutingId target, Object message);
    ZLinkRequestCall requestToNode(
        String meshName, RoutingId target, Object request);
    ZLinkSendCall sendToChannel(String channelName, Object message);
    ZLinkRequestCall requestToChannel(String channelName, Object request);
    ZLinkSpotSendCall sendToSpot(String spotId, Object message);
    ZLinkSpotRequestCall requestToSpot(String spotId, Object request);
}

public interface ZLinkFanoutClient {
    ZLinkFanoutPublishCall publish(
        String channelName, String topic, Object event);
    ZLinkFanoutPublishCall publish(String channelName, Object event);
}

public enum ZLinkRequestFailureReason {
    TIMEOUT, CANCELLED, SHUTDOWN
}

public final class ZLinkRequestFailureException extends RuntimeException {
    public ZLinkRequestFailureException(
        ZLinkRequestFailureReason reason, String message);
    public ZLinkRequestFailureException(
        ZLinkRequestFailureReason reason, String message, Throwable cause);
    public ZLinkRequestFailureReason reason();
}

```

A Channel call only uses the process-local
[ChannelName](../../../../01-glossary.en.md#channelname) index. An
overload where the caller passes MeshName and ChannelName together to
choose the physical wiring isn't provided. `sendToNode(String, RoutingId,
Object)` specifies an exact RID, so its first argument is interpreted as
[MeshName](../../../../01-glossary.en.md#meshname).

A Spot direct operation only takes the global SpotId as address and
returns a Spot-dedicated fluent call. That call's `instanceSpot()` marker
and optional stable type/initial Mesh express the cold-activation intent
for a Missing Instance Spot. Without the marker, Missing authority ends
as not-found. Existing [authority](../../../../01-glossary.en.md#authority)
uses the stored kind/stable type and current owner, so it doesn't require
type or Mesh again. The detailed members and
[cold activation](../../../../01-glossary.en.md#cold-activation) selection
rules are owned by the [Java Spot Interface](spots.en.md).

On the [RouteMesh](../../../../01-glossary.en.md#routemesh) builder
`channel(channelName)` returns, select exactly one of `client()` or
`server()`. The builder of `addClientServerChannel(channelName)` can
register one or both of the two roles, but each role at most once.
Client and Server of the same ChannelName share one ClientServer
topology but are separate registrations under the `(ChannelName, Role)`
key. A duplicate registration of the same role is a startup error. The
RouteMesh ChannelName conflict rule stays the same. Weight and handler
can't be set before selecting the role, and Server settings only exist on
each Server builder.

RouteMesh Channel Server, ClientServer Server, and node-wide placement
weight are all `int`, in range `0..10000`, defaulting to `100`. An
out-of-range value is a configuration error in both startup config and
runtime change. Weighted selection computes the sum of candidate weight
using at least a 64-bit integer.

ClientServer's local Server is also included in the same candidate set
as a remote Server once it finishes listener and service admission. The
same Ready, positive [weight](../../../../01-glossary.en.md#weight), and
non-draining conditions apply — there's no local priority or remote
exclusion rule. Even when a local Server is selected, the actual
transport message is delivered from the Client DEALER to the Server
ROUTER, without bypassing codec, HWM, timeout, cancellation, correlation,
or terminal completion.

Specifying the internal liveness-dedicated exact topic byte `01 5A 4C 46
31` in `ZLinkFanoutClient.publish(...)` raises
`ZLinkConfigurationException` without starting transport. The overload
that omits [topic](../../../../01-glossary.en.md#topic) uses the typed
event's packet name, so it doesn't create this internal topic.

Behind the public call, the framework only uses the Java binding's
exported raw socket API. Core service objects, private dispatch records,
and native handles aren't exposed in handler context, call result, or
exception payload.

In the following example, `client` is a `ZLinkClient` obtained through
configuration or dependency injection. It starts a request with
ChannelName, and the stage `submit(...)` returns waits until the
terminal reply.

```java
CompletionStage<CheckoutReply> reply = client
    .requestToChannel("checkout", request) // the framework selects one of the checkout Server candidates.
    .timeout(Duration.ofSeconds(5))        // specifies the timeout applied to this request operation.
    .submit(CheckoutReply.class);          // specifies the reply type and waits for the terminal reply.
```

## Exact Public Member Inventory

The declarations below fix this category's Java public types and
members.

```java
public final class systems.zlink.framework.channels.ZLinkRequestFailureReason extends java.lang.Enum<systems.zlink.framework.channels.ZLinkRequestFailureReason> {
  public static final systems.zlink.framework.channels.ZLinkRequestFailureReason TIMEOUT;
  public static final systems.zlink.framework.channels.ZLinkRequestFailureReason CANCELLED;
  public static final systems.zlink.framework.channels.ZLinkRequestFailureReason SHUTDOWN;
  public static systems.zlink.framework.channels.ZLinkRequestFailureReason[] values();
  public static systems.zlink.framework.channels.ZLinkRequestFailureReason valueOf(java.lang.String);
}
public final class systems.zlink.framework.channels.ZLinkRequestFailureException extends java.lang.RuntimeException {
  public systems.zlink.framework.channels.ZLinkRequestFailureException(systems.zlink.framework.channels.ZLinkRequestFailureReason, java.lang.String);
  public systems.zlink.framework.channels.ZLinkRequestFailureException(systems.zlink.framework.channels.ZLinkRequestFailureReason, java.lang.String, java.lang.Throwable);
  public systems.zlink.framework.channels.ZLinkRequestFailureReason reason();
}
public interface systems.zlink.framework.configuration.ZLinkMeshChannelBuilder {
  public abstract systems.zlink.framework.configuration.ZLinkMeshChannelClientBuilder client();
  public abstract systems.zlink.framework.configuration.ZLinkMeshChannelServerBuilder server();
}
public interface systems.zlink.framework.configuration.ZLinkMeshChannelClientBuilder {
}
public interface systems.zlink.framework.configuration.ZLinkMeshChannelServerBuilder {
  public abstract systems.zlink.framework.configuration.ZLinkMeshChannelServerBuilder setWeight(int);
  public abstract systems.zlink.framework.configuration.ZLinkMeshChannelServerBuilder addHandlerGroup(java.lang.String);
  public abstract <THandler extends systems.zlink.framework.channels.ZLinkSendHandler<TMessage>, TMessage> systems.zlink.framework.configuration.ZLinkMeshChannelServerBuilder addSendHandler(java.lang.Class<THandler>, java.lang.Class<TMessage>);
  public abstract <THandler extends systems.zlink.framework.channels.ZLinkRequestHandler<TRequest, TReply>, TRequest, TReply> systems.zlink.framework.configuration.ZLinkMeshChannelServerBuilder addRequestHandler(java.lang.Class<THandler>, java.lang.Class<TRequest>, java.lang.Class<TReply>);
}
public interface systems.zlink.framework.configuration.ZLinkClientServerChannelClientBuilder {
  public abstract systems.zlink.framework.configuration.ZLinkClientServerChannelClientBuilder connect(java.lang.String);
}
public interface systems.zlink.framework.configuration.ClientServerChannelBuilder {
  public abstract systems.zlink.framework.configuration.ZLinkClientServerChannelClientBuilder client();
  public abstract systems.zlink.framework.configuration.ZLinkClientServerChannelServerBuilder server();
}
public interface systems.zlink.framework.configuration.ZLinkClientServerChannelServerBuilder {
  public abstract systems.zlink.framework.configuration.ZLinkClientServerChannelServerBuilder listen();
  public abstract systems.zlink.framework.configuration.ZLinkClientServerChannelServerBuilder listen(int);
  public abstract systems.zlink.framework.configuration.ZLinkClientServerChannelServerBuilder setBindHost(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkClientServerChannelServerBuilder setAdvertiseHost(java.lang.String);
  public abstract systems.zlink.framework.configuration.ZLinkClientServerChannelServerBuilder setWeight(int);
  public abstract systems.zlink.framework.configuration.ZLinkClientServerChannelServerBuilder addHandlerGroup(java.lang.String);
  public abstract <THandler extends systems.zlink.framework.channels.ZLinkSendHandler<TMessage>, TMessage> systems.zlink.framework.configuration.ZLinkClientServerChannelServerBuilder addSendHandler(java.lang.Class<THandler>, java.lang.Class<TMessage>);
  public abstract <THandler extends systems.zlink.framework.channels.ZLinkRequestHandler<TRequest, TReply>, TRequest, TReply> systems.zlink.framework.configuration.ZLinkClientServerChannelServerBuilder addRequestHandler(java.lang.Class<THandler>, java.lang.Class<TRequest>, java.lang.Class<TReply>);
}
public interface systems.zlink.framework.channels.ZLinkClient {
  public abstract systems.zlink.framework.channels.ZLinkSendCall sendToChannel(java.lang.String, java.lang.Object);
  public abstract systems.zlink.framework.channels.ZLinkRequestCall requestToChannel(java.lang.String, java.lang.Object);
}
public interface systems.zlink.framework.channels.ZLinkFanoutClient {
  public abstract systems.zlink.framework.channels.ZLinkFanoutPublishCall publish(java.lang.String, java.lang.String, java.lang.Object);
  public abstract systems.zlink.framework.channels.ZLinkFanoutPublishCall publish(java.lang.String, java.lang.Object);
}
public interface systems.zlink.framework.channels.ZLinkFanoutPublishCall {
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> submit();
}
public interface systems.zlink.framework.channels.ZLinkMeshChannelRuntimeOptions {
  public abstract int weight();
  public abstract void weight(int);
}
public interface systems.zlink.framework.channels.ZLinkPublishCall {
  public default systems.zlink.framework.channels.ZLinkPublishCall metadata(java.lang.String, java.lang.String);
  public default systems.zlink.framework.channels.ZLinkPublishCall metadata(java.util.Map<java.lang.String, java.lang.String>);
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> submit();
}
public interface systems.zlink.framework.channels.ZLinkPublishMessageContext extends systems.zlink.framework.ZLinkMessageContext {
  public abstract java.lang.String topic();
  public abstract java.util.Optional<java.lang.String> source();
}
public interface systems.zlink.framework.channels.ZLinkFanoutHandler<TMessage> {
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> handle(TMessage, systems.zlink.framework.channels.ZLinkPublishMessageContext);
}
public interface systems.zlink.framework.channels.ZLinkRequestCall {
  public default systems.zlink.framework.channels.ZLinkRequestCall metadata(java.lang.String, java.lang.String);
  public default systems.zlink.framework.channels.ZLinkRequestCall metadata(java.util.Map<java.lang.String, java.lang.String>);
  public abstract systems.zlink.framework.channels.ZLinkRequestCall timeout(java.time.Duration);
  public abstract <TReply> java.util.concurrent.CompletionStage<TReply> submit(java.lang.Class<TReply>);
  public abstract <TReply> java.util.concurrent.CompletionStage<TReply> yield(java.lang.Class<TReply>);
}
public interface systems.zlink.framework.channels.ZLinkRequestHandler<TRequest, TReply> {
  public abstract java.util.concurrent.CompletionStage<TReply> handle(TRequest, systems.zlink.framework.ZLinkMessageContext);
}
public interface systems.zlink.framework.channels.ZLinkRouteClient {
  public abstract systems.zlink.framework.channels.ZLinkSendCall sendToChannel(java.lang.String, java.lang.Object);
  public abstract systems.zlink.framework.channels.ZLinkRequestCall requestToChannel(java.lang.String, java.lang.Object);
  public abstract systems.zlink.framework.channels.ZLinkSendCall sendToNode(java.lang.String, systems.zlink.contracts.core.RoutingId, java.lang.Object);
  public abstract systems.zlink.framework.spots.ZLinkSpotSendCall sendToSpot(java.lang.String, java.lang.Object);
  public abstract systems.zlink.framework.channels.ZLinkRequestCall requestToNode(java.lang.String, systems.zlink.contracts.core.RoutingId, java.lang.Object);
  public abstract systems.zlink.framework.spots.ZLinkSpotRequestCall requestToSpot(java.lang.String, java.lang.Object);
}
public interface systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions {
  public abstract systems.zlink.framework.channels.ZLinkMeshChannelRuntimeOptions channel(java.lang.String, java.lang.String);
  public abstract systems.zlink.framework.channels.ZLinkMeshPlacementRuntimeOptions mesh(java.lang.String);
  public abstract systems.zlink.framework.channels.ZLinkMeshChannelRuntimeOptions channel(java.lang.String);
}
public interface systems.zlink.framework.channels.ZLinkMeshPlacementRuntimeOptions {
  public abstract int placementWeight();
  public abstract void setPlacementWeight(int);
}
public interface systems.zlink.framework.channels.ZLinkRouteMessageContext extends systems.zlink.framework.ZLinkMessageContext {
  public abstract systems.zlink.contracts.core.RoutingId sourceNodeRid();
}
public interface systems.zlink.framework.channels.ZLinkRouteRequestHandler<TRequest, TReply> {
  public abstract java.util.concurrent.CompletionStage<TReply> handle(TRequest, systems.zlink.framework.channels.ZLinkRouteMessageContext);
}
public interface systems.zlink.framework.channels.ZLinkRouteSendHandler<TMessage> {
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> handle(TMessage, systems.zlink.framework.channels.ZLinkRouteMessageContext);
}
public interface systems.zlink.framework.channels.ZLinkSendCall {
  public default systems.zlink.framework.channels.ZLinkSendCall metadata(java.lang.String, java.lang.String);
  public default systems.zlink.framework.channels.ZLinkSendCall metadata(java.util.Map<java.lang.String, java.lang.String>);
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> submit();
}
public interface systems.zlink.framework.channels.ZLinkSendHandler<TMessage> {
  public abstract java.util.concurrent.CompletionStage<java.lang.Void> handle(TMessage, systems.zlink.framework.ZLinkMessageContext);
}
public interface systems.zlink.framework.handlers.ZLinkHandlerGroup extends java.lang.annotation.Annotation {
  public abstract java.lang.String value();
}
public interface systems.zlink.framework.handlers.ZLinkHandlerGroups extends java.lang.annotation.Annotation {
  public abstract systems.zlink.framework.handlers.ZLinkHandlerGroup[] value();
}
public interface systems.zlink.framework.handlers.ZLinkPacket extends java.lang.annotation.Annotation {
  public abstract java.lang.String value();
}
public interface systems.zlink.framework.handlers.ZLinkPublish extends java.lang.annotation.Annotation {
  public abstract java.lang.String packetName();
}
public interface systems.zlink.framework.handlers.ZLinkRequest extends java.lang.annotation.Annotation {
  public abstract java.lang.String packetName();
}
public interface systems.zlink.framework.handlers.ZLinkSend extends java.lang.annotation.Annotation {
  public abstract java.lang.String packetName();
}
public interface systems.zlink.framework.handlers.ZLinkSpotActorRequest extends java.lang.annotation.Annotation {
  public abstract java.lang.String packetName();
}
public interface systems.zlink.framework.handlers.ZLinkSpotActorSend extends java.lang.annotation.Annotation {
  public abstract java.lang.String packetName();
}
public interface systems.zlink.framework.handlers.ZLinkSpotRequest extends java.lang.annotation.Annotation {
  public abstract java.lang.String packetName();
}
public interface systems.zlink.framework.handlers.ZLinkSpotSubscription extends java.lang.annotation.Annotation {
  public abstract java.lang.String spotNodeName();
  public abstract java.lang.String topic();
}
public interface systems.zlink.framework.handlers.ZLinkSpotTimer extends java.lang.annotation.Annotation {
  public abstract java.lang.String name();
  public abstract long periodMillis();
}
public interface systems.zlink.framework.handlers.ZLinkStreamPacket extends java.lang.annotation.Annotation {
}
public interface systems.zlink.framework.handlers.ZLinkStreamRaw extends java.lang.annotation.Annotation {
}
```

`yield(...)` declared on this document's request builder is only valid
when the caller owns the shared turn of a `SpotWide` User Spot or
Instance Spot. In a different execution context, it completes with
`INVALID_OPERATION`, without submitting the message or returning the
turn. `submit(...)` is the common `Async` semantics that keeps the
current turn.

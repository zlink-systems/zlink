package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinRequest;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchHandler;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchInfo;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalAsyncSpotDispatchHandler;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;

import java.time.Duration;
import java.util.List;
import java.util.Objects;
import java.util.Queue;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentLinkedQueue;

/**
 * Framework-owned local Spot mailbox. Raw bindings provide transport only; Spot identity, lifecycle
 * and turn dispatch stay in the Framework runtime.
 */
final class ZLinkJavaRawSpot implements ZLinkBackendSpot, ZLinkJavaAdmissionBacked {
    private final ZLinkJavaRawSpotNode owner;
    private final long lifecycleGeneration;
    private final Queue<ZLinkBackendReceived> routes = new ConcurrentLinkedQueue<>();
    private final Queue<ZLinkBackendTopicMessage> subscriptions = new ConcurrentLinkedQueue<>();
    private final Set<String> topics = ConcurrentHashMap.newKeySet();
    private volatile String spotId;
    private volatile ZLinkBackendSpotDispatchHandler dispatchHandler;

    ZLinkJavaRawSpot(ZLinkJavaRawSpotNode owner, String spotId, long lifecycleGeneration) {
        this.owner = owner;
        this.spotId = spotId;
        this.lifecycleGeneration = lifecycleGeneration;
    }

    @Override
    public String name() {
        return "rawSpot." + spotId;
    }

    @Override
    public String spotId() {
        return spotId;
    }

    @Override
    public long lifecycleGeneration() {
        return lifecycleGeneration;
    }

    @Override
    public boolean closeInstanceSpot() {
        return owner.closeInstanceSpot(spotId, lifecycleGeneration);
    }

    @Override
    public void setRoutingId(String value) {
        owner.rekeySpot(this, spotId, Objects.requireNonNull(value, "spotId"));
        spotId = value;
    }

    @Override
    public void setSubscription(String topic) {
        if (topic == null || topic.isBlank()) {
            throw new IllegalArgumentException("topic is required");
        }
        topics.add(topic);
    }

    @Override
    public ZLinkBackendTopicMessage subscribe(ZLinkBackendRecvMode mode) {
        return subscriptions.poll();
    }

    @Override
    public ZLinkBackendReceived recvRoute(ZLinkBackendRecvMode mode) {
        return routes.poll();
    }

    @Override
    public void rememberSpotAuthority(
            RoutingId targetNodeRid,
            String spotId,
            long objectGeneration,
            long authorityOwnerGeneration) {
        owner.rememberSpotAuthority(
                targetNodeRid, spotId, objectGeneration, authorityOwnerGeneration);
    }

    @Override
    public void rememberSpotAuthority(
            RoutingId targetNodeRid,
            String spotId,
            long objectGeneration,
            long authorityOwnerGeneration,
            long ownerLeaseGeneration) {
        owner.rememberSpotAuthority(
                targetNodeRid,
                spotId,
                objectGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration);
    }

    @Override
    public boolean publish(String channelName, String topic, List<Message> parts, SendFlags flags) {
        return owner.publish(this, channelName, topic, new byte[0], parts);
    }

    @Override
    public boolean publish(
            String channelName,
            String topic,
            byte[] metadata,
            List<Message> parts,
            SendFlags flags) {
        return owner.publish(this, channelName, topic, metadata, parts);
    }

    @Override
    public CompletionStage<Void> publishAsync(
            String channelName, String topic, List<Message> parts, SendFlags flags) {
        return owner.publishAsync(this, channelName, topic, new byte[0], parts);
    }

    @Override
    public CompletionStage<Void> publishAsync(
            String channelName,
            String topic,
            byte[] metadata,
            List<Message> parts,
            SendFlags flags) {
        return owner.publishAsync(this, channelName, topic, metadata, parts);
    }

    @Override
    public CompletionStage<Void> sendToSpot(
            RoutingId targetNodeRid, String spotId, long spotGeneration, List<Message> parts) {
        return owner.sendToSpot(this, targetNodeRid, spotId, spotGeneration, new byte[0], parts);
    }

    @Override
    public CompletionStage<Void> sendToSpot(
            RoutingId targetNodeRid,
            String spotId,
            long spotGeneration,
            byte[] metadata,
            List<Message> parts) {
        return owner.sendToSpot(this, targetNodeRid, spotId, spotGeneration, metadata, parts);
    }

    @Override
    public CompletionStage<ZLinkBackendReceived> requestToSpot(
            RoutingId targetNodeRid,
            String spotId,
            long spotGeneration,
            List<Message> parts,
            Duration timeout) {
        return owner.requestToSpot(
                this, targetNodeRid, spotId, spotGeneration, new byte[0], parts, timeout);
    }

    @Override
    public CompletionStage<ZLinkBackendReceived> requestToSpot(
            RoutingId targetNodeRid,
            String spotId,
            long spotGeneration,
            byte[] metadata,
            List<Message> parts,
            Duration timeout,
            ZLinkServiceOperationRegistry operations,
            UUID operationId) {
        return owner.requestToSpot(
                this,
                targetNodeRid,
                spotId,
                spotGeneration,
                metadata,
                parts,
                timeout,
                operations,
                operationId);
    }

    @Override
    public CompletionStage<ZLinkBackendReceived> requestToSpot(
            RoutingId targetNodeRid,
            String spotId,
            long spotGeneration,
            byte[] metadata,
            List<Message> parts,
            Duration timeout) {
        return owner.requestToSpot(
                this, targetNodeRid, spotId, spotGeneration, metadata, parts, timeout);
    }

    @Override
    public void onDispatchEvent(ZLinkBackendSpotDispatchHandler handler) {
        dispatchHandler = handler;
    }

    @Override
    public ZLinkBackendActorJoinRequest recvActorJoin(ZLinkBackendRecvMode mode) {
        return null;
    }

    @Override
    public void replyActorJoin(
            ZLinkBackendActorJoinRequest request, int joinResultCode, List<Message> parts) {
        throw new UnsupportedOperationException(
                "raw Spot local Actor Join records are not supported");
    }

    boolean accepts(String topic) {
        return topics.contains(topic);
    }

    CompletionStage<Void> enqueueRoute(ZLinkBackendReceived received) {
        return enqueueRoute(received, null);
    }

    CompletionStage<Void> enqueueRoute(
            ZLinkBackendReceived received, CompletableFuture<Void> admission) {
        ZLinkBackendSpotDispatchHandler handler = dispatchHandler;
        if (handler instanceof ZLinkInternalAsyncSpotDispatchHandler async) {
            CompletionStage<Void> direct =
                    admission == null
                            ? async.handleRoute(received)
                            : async.handleRoute(received, admission);
            if (direct != null) {
                return direct;
            }
        }
        IllegalStateException detached = null;
        synchronized (this) {
            if (owner.localSpot(spotId) != this) {
                received.close();
                detached = new IllegalStateException("target Spot is closed");
            } else {
                routes.add(received);
            }
        }
        if (detached != null) {
            if (admission != null) admission.completeExceptionally(detached);
            return CompletableFuture.failedFuture(detached);
        }
        if (admission != null) admission.complete(null);
        return raise(ZLinkBackendSpotDispatchEvent.ROUTED_READABLE);
    }

    boolean enqueueTopic(ZLinkBackendTopicMessage message) {
        ZLinkBackendSpotDispatchHandler handler = dispatchHandler;
        if (handler instanceof ZLinkInternalAsyncSpotDispatchHandler async) {
            Boolean direct = async.handleTopic(message);
            if (direct != null) {
                return direct;
            }
        }
        synchronized (this) {
            if (owner.localSpot(spotId) != this) {
                message.parts().forEach(Message::close);
                return false;
            }
            subscriptions.add(message);
        }
        raise(ZLinkBackendSpotDispatchEvent.SUBSCRIBE_READABLE);
        return true;
    }

    CompletionStage<Void> enqueueActor(List<ZLinkBackendActorReceived> messages) {
        ZLinkBackendSpotDispatchHandler handler = dispatchHandler;
        if (handler instanceof ZLinkInternalAsyncSpotDispatchHandler async) {
            CompletionStage<Void> direct = async.handleActor(messages);
            if (direct != null) {
                return direct;
            }
        }
        synchronized (this) {
            if (owner.localSpot(spotId) != this) {
                messages.forEach(ZLinkBackendActorReceived::close);
                return CompletableFuture.failedFuture(
                        new IllegalStateException("target Spot is closed"));
            }
        }
        return raise(ZLinkBackendSpotDispatchEvent.ACTOR_READABLE, messages);
    }

    private CompletionStage<Void> raise(ZLinkBackendSpotDispatchEvent event) {
        return raise(event, List.of());
    }

    private CompletionStage<Void> raise(
            ZLinkBackendSpotDispatchEvent event, List<ZLinkBackendActorReceived> actorMessages) {
        ZLinkBackendSpotDispatchHandler handler = dispatchHandler;
        if (handler == null) {
            actorMessages.forEach(ZLinkBackendActorReceived::close);
            return CompletableFuture.completedFuture(null);
        }
        ZLinkBackendSpotDispatchInfo info = new ZLinkBackendSpotDispatchInfo(event, actorMessages);
        if (handler instanceof ZLinkInternalAsyncSpotDispatchHandler async) {
            return async.handleAsync(info);
        }
        handler.handle(info);
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public void close() {
        synchronized (this) {
            if (owner.localSpot(spotId) != this) {
                return;
            }
            owner.removeSpot(this);
        }
        ZLinkBackendReceived route;
        while ((route = routes.poll()) != null) {
            route.close();
        }
        ZLinkBackendTopicMessage topic;
        while ((topic = subscriptions.poll()) != null) {
            topic.parts().forEach(Message::close);
        }
    }

    static List<Message> copy(List<Message> parts) {
        return parts.stream().map(Message::from).toList();
    }
}

package systems.zlink.framework.runtime.binding;

import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.Queue;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.atomic.AtomicBoolean;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinRequest;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorLifecycleEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestCallback;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchHandler;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchInfo;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalAsyncSpotDispatchHandler;

/**
 * Framework-owned local Spot mailbox. Raw bindings provide transport only;
 * Spot identity, lifecycle and turn dispatch stay in the Framework runtime.
 */
final class ZLinkJavaRawSpot
    implements ZLinkBackendSpot, ZLinkJavaAdmissionBacked {
    private final ZLinkJavaRawSpotNode owner;
    private final long lifecycleGeneration;
    private final Queue<ZLinkBackendReceived> routes =
        new ConcurrentLinkedQueue<>();
    private final Queue<ZLinkBackendTopicMessage> subscriptions =
        new ConcurrentLinkedQueue<>();
    private final Queue<ZLinkBackendActorJoinRequest> actorJoins =
        new ConcurrentLinkedQueue<>();
    private final Queue<ZLinkBackendActorLifecycleEvent> lifecycles =
        new ConcurrentLinkedQueue<>();
    private final Set<String> topics = ConcurrentHashMap.newKeySet();
    private final AtomicBoolean closed = new AtomicBoolean();
    private volatile String spotId;
    private volatile ZLinkBackendSpotDispatchHandler dispatchHandler;

    ZLinkJavaRawSpot(
        ZLinkJavaRawSpotNode owner,
        String spotId,
        long lifecycleGeneration) {
        this.owner = owner;
        this.spotId = spotId;
        this.lifecycleGeneration = lifecycleGeneration;
    }

    @Override
    public String name() {
        return "rawSpot." + spotId;
    }

    @Override
    public ZLinkBackendObject admissionSource() {
        return owner;
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
        owner.rekeySpot(this, spotId, java.util.Objects.requireNonNull(
            value, "spotId"));
        spotId = value;
    }

    @Override
    public void setSubscription(String topic) {
        if (topic == null || topic.isBlank()) {
            throw new IllegalArgumentException("topic is required");
        }
        topics.add(topic);
        owner.streamTrace("spot-subscription-bind spot=" + spotId
            + " topic=" + topic);
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
            targetNodeRid,
            spotId,
            objectGeneration,
            authorityOwnerGeneration);
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
    public boolean publish(
        String channelName,
        String topic,
        List<Message> parts,
        SendFlags flags) {
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
    public boolean sendToSpot(
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        List<Message> parts,
        SendFlags flags) {
        return owner.sendToSpot(
            this, targetNodeRid, spotId, spotGeneration, new byte[0], parts);
    }

    @Override
    public boolean sendToSpot(
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        byte[] metadata,
        List<Message> parts,
        SendFlags flags) {
        return owner.sendToSpot(
            this, targetNodeRid, spotId, spotGeneration, metadata, parts);
    }

    @Override
    public boolean requestToSpot(
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        List<Message> parts,
        ZLinkBackendRequestCallback callback,
        SendFlags flags,
        Duration timeout) {
        return owner.requestToSpot(
            this,
            targetNodeRid,
            spotId,
            spotGeneration,
            new byte[0],
            parts,
            callback,
            timeout);
    }

    @Override
    public boolean requestToSpot(
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        byte[] metadata,
        List<Message> parts,
        ZLinkBackendRequestCallback callback,
        SendFlags flags,
        Duration timeout) {
        return owner.requestToSpot(
            this,
            targetNodeRid,
            spotId,
            spotGeneration,
            metadata,
            parts,
            callback,
            timeout);
    }

    @Override
    public void onDispatchEvent(ZLinkBackendSpotDispatchHandler handler) {
        dispatchHandler = handler;
    }

    @Override
    public ZLinkBackendActorJoinRequest recvActorJoin(ZLinkBackendRecvMode mode) {
        return actorJoins.poll();
    }

    @Override
    public void replyActorJoin(
        ZLinkBackendActorJoinRequest request,
        int joinResultCode,
        List<Message> parts) {
        if (!(request.nativeRequest() instanceof PendingJoin pending)) {
            throw new IllegalArgumentException("unknown raw Spot join request");
        }
        pending.complete(joinResultCode, copy(parts));
    }

    @Override
    public ZLinkBackendActorLifecycleEvent recvActorLifecycle(
        ZLinkBackendRecvMode mode) {
        return lifecycles.poll();
    }

    boolean accepts(String topic) {
        return topics.contains(topic);
    }

    CompletionStage<Void> enqueueRoute(ZLinkBackendReceived received) {
        if (closed.get()) {
            received.close();
            return CompletableFuture.failedFuture(
                new IllegalStateException("target Spot is closed"));
        }
        routes.add(received);
        return raise(ZLinkBackendSpotDispatchEvent.ROUTED_READABLE);
    }

    boolean enqueueTopic(ZLinkBackendTopicMessage message) {
        if (closed.get()) {
            message.parts().forEach(Message::close);
            message.closeAdmission();
            return false;
        }
        subscriptions.add(message);
        raise(ZLinkBackendSpotDispatchEvent.SUBSCRIBE_READABLE);
        return true;
    }

    CompletionStage<Void> enqueueJoin(ZLinkBackendActorJoinRequest request) {
        if (closed.get()) {
            request.parts().forEach(Message::close);
            return CompletableFuture.failedFuture(
                new IllegalStateException("target Spot is closed"));
        }
        actorJoins.add(request);
        return raise(ZLinkBackendSpotDispatchEvent.ACTOR_JOIN_READABLE);
    }

    CompletionStage<Void> enqueueLifecycle(
        ZLinkBackendActorLifecycleEvent event) {
        if (closed.get()) {
            return CompletableFuture.completedFuture(null);
        }
        lifecycles.add(event);
        return raise(ZLinkBackendSpotDispatchEvent.ACTOR_LIFECYCLE_READABLE);
    }

    CompletionStage<Void> enqueueActor(
        List<ZLinkBackendActorReceived> messages) {
        owner.streamTrace("mailbox actor enqueue spot=" + spotId
            + " closed=" + closed.get()
            + " handler=" + (dispatchHandler != null)
            + " messages=" + messages.size());
        if (closed.get()) {
            messages.forEach(ZLinkBackendActorReceived::close);
            return CompletableFuture.failedFuture(
                new IllegalStateException("target Spot is closed"));
        }
        CompletionStage<Void> raised = raise(
            ZLinkBackendSpotDispatchEvent.ACTOR_READABLE, messages);
        raised.whenComplete((ignored, error) -> owner.streamTrace(
            "mailbox actor dispatch-complete spot=" + spotId
                + " error=" + (error == null ? "none" : error)));
        return raised;
    }

    private CompletionStage<Void> raise(ZLinkBackendSpotDispatchEvent event) {
        return raise(event, List.of());
    }

    private CompletionStage<Void> raise(
        ZLinkBackendSpotDispatchEvent event,
        List<ZLinkBackendActorReceived> actorMessages) {
        ZLinkBackendSpotDispatchHandler handler = dispatchHandler;
        if (handler == null) {
            actorMessages.forEach(ZLinkBackendActorReceived::close);
            return CompletableFuture.completedFuture(null);
        }
        ZLinkBackendSpotDispatchInfo info =
            new ZLinkBackendSpotDispatchInfo(event, actorMessages);
        if (handler instanceof ZLinkInternalAsyncSpotDispatchHandler async) {
            return async.handleAsync(info);
        }
        handler.handle(info);
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public void close() {
        if (!closed.compareAndSet(false, true)) {
            return;
        }
        ZLinkBackendReceived route;
        while ((route = routes.poll()) != null) {
            route.close();
        }
        ZLinkBackendTopicMessage topic;
        while ((topic = subscriptions.poll()) != null) {
            topic.parts().forEach(Message::close);
            topic.closeAdmission();
        }
        ZLinkBackendActorJoinRequest join;
        while ((join = actorJoins.poll()) != null) {
            join.parts().forEach(Message::close);
            if (join.nativeRequest() instanceof PendingJoin pending) {
                pending.fail(new IllegalStateException("target Spot is closed"));
            }
        }
        lifecycles.clear();
        owner.removeSpot(this);
    }

    static List<Message> copy(List<Message> parts) {
        return parts.stream()
            .map(part -> Message.from(part.toByteArray()))
            .toList();
    }

    static final class PendingJoin {
        private final CompletableFuture<JoinReply> completion =
            new CompletableFuture<>();
        private final AtomicBoolean terminal = new AtomicBoolean();

        CompletionStage<JoinReply> completion() {
            return completion;
        }

        void complete(int resultCode, List<Message> parts) {
            if (!terminal.compareAndSet(false, true)) {
                parts.forEach(Message::close);
                throw new IllegalStateException("actor join already completed");
            }
            completion.complete(new JoinReply(resultCode, parts));
        }

        void fail(Throwable failure) {
            if (terminal.compareAndSet(false, true)) {
                completion.completeExceptionally(failure);
            }
        }
    }

    record JoinReply(int resultCode, List<Message> parts) {
    }
}

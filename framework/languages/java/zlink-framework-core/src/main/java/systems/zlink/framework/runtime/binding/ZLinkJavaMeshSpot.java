package systems.zlink.framework.runtime.binding;

import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.Queue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentLinkedQueue;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.binding.spot.ActorJoinDecision;
import systems.zlink.framework.runtime.internal.binding.spot.Dispatch;
import systems.zlink.framework.runtime.internal.binding.spot.RecordKind;
import systems.zlink.framework.runtime.internal.binding.spot.ReplyToken;
import systems.zlink.framework.runtime.internal.binding.spot.Spot;
import systems.zlink.framework.runtime.internal.binding.spot.SubscriptionKind;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinRequest;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorLifecycleEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorLifecycleEventKind;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
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
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.channels.ZLinkChannelContentTypeFrame;

final class ZLinkJavaMeshSpot
    implements ZLinkBackendSpot, ZLinkJavaAdmissionBacked {

    private final ZLinkJavaMeshNode owner;
    private final Spot spot;
    private final java.util.function.Supplier<String> channelName;
    private final Queue<ZLinkBackendReceived> routes = new ConcurrentLinkedQueue<>();
    private final Queue<ZLinkBackendTopicMessage> subscriptions = new ConcurrentLinkedQueue<>();
    private final Queue<ZLinkBackendActorJoinRequest> actorJoins = new ConcurrentLinkedQueue<>();
    private final Queue<ZLinkBackendActorLifecycleEvent> lifecycles =
        new ConcurrentLinkedQueue<>();
    private volatile ZLinkBackendSpotDispatchHandler dispatchHandler;

    ZLinkJavaMeshSpot(
        ZLinkJavaMeshNode owner,
        Spot spot,
        java.util.function.Supplier<String> channelName) {
        this.owner = owner;
        this.spot = spot;
        this.channelName = channelName;
    }

    @Override public String name() { return "meshSpot." + spotId(); }
    @Override public ZLinkBackendObject admissionSource() { return owner.spotNode(); }
    @Override public String spotId() { return spot.spotId(); }
    @Override public long lifecycleGeneration() { return spot.status().lifecycleGeneration(); }

    @Override
    public void setRoutingId(String spotId) {
        if (!spot.spotId().equals(spotId)) {
            throw new IllegalStateException(
                "MeshNode Spot id is assigned when the Spot is created");
        }
    }

    @Override
    public void setSubscription(String topic) {
        spot.setSubscription(requireChannel(topic), topic, SubscriptionKind.EXACT);
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
    public boolean publish(
        String channelName,
        String topic,
        List<Message> parts,
        SendFlags flags) {
        spot.publish(channelName, topic, parts, flags);
        return true;
    }

    @Override
    public boolean publish(
        String channelName,
        String topic,
        byte[] metadata,
        List<Message> parts,
        SendFlags flags) {
        spot.publish(channelName, topic, metadata, parts, flags);
        return true;
    }

    @Override
    public boolean sendToSpot(
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        List<Message> parts,
        SendFlags flags) {
        spot.sendToSpot(
            targetNodeRid,
            spotId,
            requireGeneration(targetNodeRid, spotId, spotGeneration),
            parts,
            flags);
        return true;
    }

    @Override
    public boolean sendToSpot(
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        byte[] metadata,
        List<Message> parts,
        SendFlags flags) {
        spot.sendToSpot(
            targetNodeRid,
            spotId,
            requireGeneration(targetNodeRid, spotId, spotGeneration),
            metadata,
            parts,
            flags);
        return true;
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
        var generation = requireGeneration(targetNodeRid, spotId, spotGeneration);
        systems.zlink.framework.runtime.internal.binding.spot.OperationId operation;
        try {
            operation = spot.requestToSpot(
                targetNodeRid,
                spotId,
                generation,
                parts,
                flags,
                timeout);
        } catch (systems.zlink.contracts.errors.ZlinkSubmitException error) {
            throw requestSubmitFailure(
                error, targetNodeRid, spotId, generation);
        }
        completeRequest(operation, callback);
        return true;
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
        var generation = requireGeneration(targetNodeRid, spotId, spotGeneration);
        systems.zlink.framework.runtime.internal.binding.spot.OperationId operation;
        try {
            operation = spot.requestToSpot(
                targetNodeRid,
                spotId,
                generation,
                metadata,
                parts,
                flags,
                timeout);
        } catch (systems.zlink.contracts.errors.ZlinkSubmitException error) {
            throw requestSubmitFailure(
                error, targetNodeRid, spotId, generation);
        }
        completeRequest(operation, callback);
        return true;
    }

    private void completeRequest(
        systems.zlink.framework.runtime.internal.binding.spot.OperationId operation,
        ZLinkBackendRequestCallback callback) {
        owner.trackCompletion(operation).whenComplete((completion, error) -> {
            if (error != null) {
                callback.handle(failedReply());
                return;
            }
            callback.handle(completionReply(completion));
        });
    }

    private static IllegalStateException requestSubmitFailure(
        systems.zlink.contracts.errors.ZlinkSubmitException error,
        RoutingId targetNodeRid,
        String targetSpotId,
        long generation) {
        return new IllegalStateException(
            "MeshNode Spot request submit failed result=" + error.getResult()
                + " errno=" + error.getNativeErrno()
                + " targetNode=" + targetNodeRid
                + " targetSpot=" + targetSpotId
                + " targetGeneration=" + Long.toUnsignedString(generation),
            error);
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
        ReplyToken token = (ReplyToken) request.nativeRequest();
        Dispatch.actorJoinReply(
            token,
            joinResultCode == 0
                ? ActorJoinDecision.ACCEPTED
                : ActorJoinDecision.REJECTED,
            parts,
            SendFlags.NONE);
    }

    @Override
    public ZLinkBackendActorLifecycleEvent recvActorLifecycle(ZLinkBackendRecvMode mode) {
        return lifecycles.poll();
    }

    CompletionStage<Void> accept(ZLinkMeshDispatchRecord record) {
        RecordKind kind = record.receive().kind();
        String contentType = record.receive().contentType() != null
            ? record.receive().contentType()
            : ZLinkChannelContentTypeFrame.decode(record.parts());
        if (kind == RecordKind.SPOT_SEND || kind == RecordKind.SPOT_REQUEST) {
            routes.add(new ZLinkBackendReceived(
                ZLinkBackendRequestResult.OK,
                Optional.ofNullable(record.receive().sourceNodeRid()),
                Optional.ofNullable(record.receive().sourceSpotId())
                    .map(RoutingId::toString),
                requestSequence(kind, record.receive().operationId()),
                record.receive().applicationMetadata(),
                new byte[0],
                record.parts(),
                record.receive().replyToken() == null
                    ? null
                    : parts -> Dispatch.reply(
                        record.receive().replyToken(),
                        parts,
                        SendFlags.NONE),
                () -> { },
                contentType,
                record.inboundDispatchLease()));
            return raise(ZLinkBackendSpotDispatchEvent.ROUTED_READABLE);
        }
        if (kind == RecordKind.SPOT_MULTICAST) {
            subscriptions.add(new ZLinkBackendTopicMessage(
                Optional.ofNullable(record.receive().sourceNodeRid()),
                record.receive().channelName(),
                record.receive().topic() == null ? "" : record.receive().topic(),
                record.receive().applicationMetadata(),
                record.parts(),
                contentType,
                record.inboundDispatchLease()));
            return raise(ZLinkBackendSpotDispatchEvent.SUBSCRIBE_READABLE);
        }
        if (kind == RecordKind.ACTOR_SEND || kind == RecordKind.ACTOR_REQUEST) {
            var actor = ZLinkJavaMeshSpotNode.backendRef(record.owner().actor());
            long requestId = kind == RecordKind.ACTOR_REQUEST
                ? owner.retainActorReply(
                    record.receive().operationId().low(),
                    record.receive().replyToken())
                : 0L;
            List<ZLinkBackendActorReceived> actorMessages =
                new java.util.ArrayList<>(record.parts().size());
            for (int index = 0; index < record.parts().size(); index++) {
                actorMessages.add(new ZLinkBackendActorReceived(
                    actor,
                    record.receive().sourceNodeRid(),
                    record.receive().sourceSpotId(),
                    Optional.empty(),
                    requestId,
                    kind == RecordKind.ACTOR_REQUEST ? 1 : 0,
                    Message.from(record.parts().get(index)),
                    index + 1 < record.parts().size(),
                    new byte[0],
                    contentType,
                    index == (record.parts().size() > 1 ? 1 : 0)
                        ? record.inboundDispatchLease()
                        : null));
            }
            if (actorMessages.isEmpty()) {
                record.close();
            } else {
                record.closeParts();
            }
            return raise(ZLinkBackendSpotDispatchEvent.ACTOR_READABLE, actorMessages);
        }
        if (kind == RecordKind.SPOT_CONTROL
            && record.receive().operationKind()
                == systems.zlink.framework.runtime.internal.binding.spot.OperationKind.ACTOR_JOIN) {
            var nativeActor = record.receive().sourceActor();
            if (nativeActor.actorId().isEmpty()
                && record.receive().actorControl() != null) {
                nativeActor = record.receive().actorControl().currentActor();
            }
            var actor = ZLinkJavaMeshSpotNode.backendRef(nativeActor);
            actorJoins.add(new ZLinkBackendActorJoinRequest(
                actor,
                actor,
                record.parts(),
                record.receive().replyToken()));
            return raise(ZLinkBackendSpotDispatchEvent.ACTOR_JOIN_READABLE);
        }
        if (kind == RecordKind.SPOT_CONTROL
            && record.receive().actorControl() != null) {
            var control = record.receive().actorControl();
            ZLinkBackendActorLifecycleEventKind lifecycleKind = switch (control.kind()) {
                case CREATED -> ZLinkBackendActorLifecycleEventKind.JOINED;
                case JOINED -> ZLinkBackendActorLifecycleEventKind.JOINED;
                case LEFT -> ZLinkBackendActorLifecycleEventKind.LEFT;
                case DISCONNECTED -> ZLinkBackendActorLifecycleEventKind.DISCONNECTED;
                default -> null;
            };
            if (lifecycleKind != null) {
                var previous = ZLinkJavaMeshSpotNode.backendRef(control.previousActor());
                var current = ZLinkJavaMeshSpotNode.backendRef(control.currentActor());
                lifecycles.add(new ZLinkBackendActorLifecycleEvent(
                    lifecycleKind,
                    new systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorLifecycleInfo(
                        previous == null || previous.actorId().isEmpty() ? null : previous,
                        current == null || current.actorId().isEmpty() ? null : current,
                        Optional.ofNullable(control.previousSpotId())
                            .map(RoutingId::toString),
                        Optional.ofNullable(control.currentSpotId())
                            .map(RoutingId::toString),
                        control.currentMembershipEpoch(),
                        control.resultCode())));
                owner.actorControl(control);
                if (lifecycleKind == ZLinkBackendActorLifecycleEventKind.LEFT) {
                } else {
                    return raise(ZLinkBackendSpotDispatchEvent.ACTOR_LIFECYCLE_READABLE);
                }
            }
            record.close();
            return CompletableFuture.completedFuture(null);
        }
        record.close();
        return CompletableFuture.completedFuture(null);
    }

    static Optional<Long> requestSequence(
        RecordKind kind,
        systems.zlink.framework.runtime.internal.binding.spot.OperationId operationId) {
        if (kind != RecordKind.SPOT_REQUEST || operationId == null) {
            return Optional.empty();
        }
        return Optional.of(operationId.low());
    }

    void lifecycle(ZLinkBackendActorLifecycleEvent event) {
        lifecycles.add(event);
        raise(ZLinkBackendSpotDispatchEvent.ACTOR_LIFECYCLE_READABLE);
    }

    void raisePendingLifecycle() {
        if (!lifecycles.isEmpty()) {
            raise(ZLinkBackendSpotDispatchEvent.ACTOR_LIFECYCLE_READABLE);
        }
    }

    private CompletionStage<Void> raise(ZLinkBackendSpotDispatchEvent event) {
        return raise(event, List.of());
    }

    private CompletionStage<Void> raise(
        ZLinkBackendSpotDispatchEvent event,
        List<ZLinkBackendActorReceived> actorMessages) {
        ZLinkBackendSpotDispatchHandler handler = dispatchHandler;
        if (handler != null) {
            ZLinkBackendSpotDispatchInfo info =
                new ZLinkBackendSpotDispatchInfo(event, actorMessages);
            if (handler instanceof ZLinkInternalAsyncSpotDispatchHandler asyncHandler) {
                return asyncHandler.handleAsync(info);
            }
            handler.handle(info);
            return CompletableFuture.completedFuture(null);
        }
        actorMessages.forEach(ZLinkBackendActorReceived::close);
        return CompletableFuture.completedFuture(null);
    }

    private ZLinkBackendReceived completionReply(ZLinkMeshDispatchRecord completion) {
        return new ZLinkBackendReceived(
            ZLinkJavaMeshSpotNode.mapResult(completion.receive().terminalResult()),
            Optional.empty(),
            Optional.empty(),
            Optional.empty(),
            completion.parts(),
            null,
            () -> { });
    }

    private static ZLinkBackendReceived failedReply() {
        return new ZLinkBackendReceived(
            ZLinkBackendRequestResult.INTERNAL_ERROR,
            Optional.empty(),
            Optional.empty(),
            Optional.empty(),
            List.of());
    }

    private String requireChannel(String fallback) {
        String value = channelName.get();
        return value == null || value.isBlank() ? fallback : value;
    }

    private long requireGeneration(
        RoutingId targetNodeRid,
        String targetSpotId,
        long suppliedGeneration) {
        long generation = suppliedGeneration > 0
            ? suppliedGeneration
            : owner.spotGeneration(targetNodeRid, targetSpotId);
        if (generation <= 0) {
            throw new IllegalArgumentException(
                "target Spot lifecycle generation is required");
        }
        return generation;
    }

    @Override
    public void close() {
        spot.close();
    }
}

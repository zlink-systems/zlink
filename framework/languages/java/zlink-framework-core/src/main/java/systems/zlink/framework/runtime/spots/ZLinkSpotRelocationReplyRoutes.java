package systems.zlink.framework.runtime.spots;

import java.time.Duration;
import java.time.Instant;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceFrozenRecordCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec;

/** Keeps source-process reply capabilities outside relocation payloads. */
final class ZLinkSpotRelocationReplyRoutes {
    private static final int MAX_ROUTES = 4_096;
    private static final Duration RETENTION = Duration.ofHours(24);

    private final Map<OperationId, Route> routes = new HashMap<>();

    synchronized Registration register(
        byte[] acceptedRecord,
        ZLinkBackendReceived received,
        String spotId,
        long objectGeneration) {
        Objects.requireNonNull(received, "received");
        var frozen = ZLinkServiceFrozenRecordCodec.decodeSpot(acceptedRecord);
        OperationId operation = new OperationId(
            frozen.operationHigh(), frozen.operationLow());
        if (frozen.replyRouteId().isEmpty()) {
            return new Registration(
                this, operation, received::close, false);
        }
        removeExpired(Instant.now());
        if (routes.size() >= MAX_ROUTES) {
            throw new IllegalStateException(
                "Spot relocation reply-route registry is full");
        }
        Consumer<List<Message>> reply = received.reply();
        if (reply == null) {
            throw new IllegalStateException(
                "accepted request has no source reply capability");
        }
        Route previous = routes.putIfAbsent(operation, new Route(
            spotId,
            objectGeneration,
            frozen.sourceOwnerId(),
            frozen.sourceOwnerLeaseGeneration(),
            frozen.sourceNodeRid(),
            frozen.sourceNodeGeneration(),
            frozen.replyRouteId().orElseThrow(),
            Instant.now().plus(RETENTION),
            parts -> {
                List<Message> messages = parts.stream()
                    .map(Message::from)
                    .toList();
                try {
                    reply.accept(messages);
                    return CompletableFuture.completedFuture(null);
                } catch (RuntimeException failure) {
                    messages.forEach(Message::close);
                    return CompletableFuture.failedFuture(failure);
                }
            }));
        if (previous != null) {
            throw new IllegalStateException(
                "duplicate accepted relocation operation identity");
        }
        return new Registration(
            this, operation, received::close, true);
    }

    synchronized Registration registerActor(
        byte[] acceptedRecord,
        String actorId,
        long objectGeneration,
        Function<List<byte[]>, CompletionStage<Void>> reply,
        Runnable relocationRelease) {
        Objects.requireNonNull(reply, "reply");
        Objects.requireNonNull(relocationRelease, "relocationRelease");
        var frozen = ZLinkServiceFrozenRecordCodec.decodeActor(acceptedRecord);
        if (!frozen.actorId().equals(actorId)
            || frozen.objectGeneration() != objectGeneration) {
            throw new IllegalArgumentException(
                "accepted Actor reply capability differs from its object fence");
        }
        OperationId operation = new OperationId(
            frozen.operationHigh(), frozen.operationLow());
        if (frozen.replyRouteId().isEmpty()) {
            return new Registration(
                this, operation, relocationRelease, false);
        }
        removeExpired(Instant.now());
        if (routes.size() >= MAX_ROUTES) {
            throw new IllegalStateException(
                "Actor relocation reply-route registry is full");
        }
        Route previous = routes.putIfAbsent(operation, new Route(
            actorId,
            objectGeneration,
            frozen.sourceOwnerId(),
            frozen.sourceOwnerLeaseGeneration(),
            frozen.sourceNodeRid(),
            frozen.sourceNodeGeneration(),
            frozen.replyRouteId().orElseThrow(),
            Instant.now().plus(RETENTION),
            parts -> reply.apply(parts.stream().map(byte[]::clone).toList())));
        if (previous != null) {
            throw new IllegalStateException(
                "duplicate accepted relocation operation identity");
        }
        return new Registration(
            this, operation, relocationRelease, true);
    }

    synchronized void completeLocal(OperationId operation) {
        routes.remove(operation);
    }

    synchronized void bindCommitted(
        byte[] acceptedRecord,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        long targetAuthorityOwnerGeneration) {
        bindCommitted(
            List.of(acceptedRecord),
            targetNodeRid,
            targetNodeGeneration,
            targetAuthorityOwnerGeneration);
    }

    synchronized void bindCommitted(
        List<byte[]> acceptedRecords,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        CommittedFence fence) {
        bindCommitted(
            acceptedRecords,
            targetNodeRid,
            targetNodeGeneration,
            fence.targetAuthorityOwnerGeneration());
        attachCanonicalFence(acceptedRecords, false, fence);
    }

    synchronized void bindCommitted(
        List<byte[]> acceptedRecords,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        long targetAuthorityOwnerGeneration) {
        Objects.requireNonNull(acceptedRecords, "acceptedRecords");
        Objects.requireNonNull(targetNodeRid, "targetNodeRid");
        if (targetNodeGeneration <= 0
            || targetAuthorityOwnerGeneration <= 0) {
            throw new IllegalArgumentException(
                "committed target reply fence must be positive");
        }
        List<Route> selected = new java.util.ArrayList<>();
        for (byte[] acceptedRecord : acceptedRecords) {
            var frozen = ZLinkServiceFrozenRecordCodec.decodeSpot(
                acceptedRecord);
            if (frozen.replyRouteId().isEmpty()) {
                continue;
            }
            OperationId operation = new OperationId(
                frozen.operationHigh(), frozen.operationLow());
            Route route = routes.get(operation);
            if (route == null
                || !route.sourceOwnerId.equals(frozen.sourceOwnerId())
                || route.sourceOwnerLeaseGeneration
                    != frozen.sourceOwnerLeaseGeneration()
                || route.committed && (!targetNodeRid.equals(
                        route.targetNodeRid)
                    || route.targetNodeGeneration != targetNodeGeneration
                    || route.targetAuthorityOwnerGeneration
                        != targetAuthorityOwnerGeneration)) {
                throw new IllegalStateException(
                    "committed relocation reply capability is unavailable");
            }
            selected.add(route);
        }
        for (Route route : selected) {
            route.targetNodeRid = targetNodeRid;
            route.targetNodeGeneration = targetNodeGeneration;
            route.targetAuthorityOwnerGeneration =
                targetAuthorityOwnerGeneration;
            route.committed = true;
        }
    }

    synchronized void bindActorCommitted(
        List<byte[]> acceptedRecords,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        long targetAuthorityOwnerGeneration) {
        Objects.requireNonNull(acceptedRecords, "acceptedRecords");
        Objects.requireNonNull(targetNodeRid, "targetNodeRid");
        if (targetNodeGeneration <= 0
            || targetAuthorityOwnerGeneration <= 0) {
            throw new IllegalArgumentException(
                "committed target reply fence must be positive");
        }
        List<Route> selected = new java.util.ArrayList<>();
        for (byte[] acceptedRecord : acceptedRecords) {
            var frozen = ZLinkServiceFrozenRecordCodec.decodeActor(
                acceptedRecord);
            if (frozen.replyRouteId().isEmpty()) {
                continue;
            }
            OperationId operation = new OperationId(
                frozen.operationHigh(), frozen.operationLow());
            Route route = routes.get(operation);
            if (route == null
                || !route.sourceOwnerId.equals(frozen.sourceOwnerId())
                || route.sourceOwnerLeaseGeneration
                    != frozen.sourceOwnerLeaseGeneration()
                || route.committed && (!targetNodeRid.equals(
                        route.targetNodeRid)
                    || route.targetNodeGeneration != targetNodeGeneration
                    || route.targetAuthorityOwnerGeneration
                        != targetAuthorityOwnerGeneration)) {
                throw new IllegalStateException(
                    "committed Actor relocation reply capability is unavailable");
            }
            selected.add(route);
        }
        for (Route route : selected) {
            route.targetNodeRid = targetNodeRid;
            route.targetNodeGeneration = targetNodeGeneration;
            route.targetAuthorityOwnerGeneration =
                targetAuthorityOwnerGeneration;
            route.committed = true;
        }
    }

    synchronized void bindActorCommitted(
        List<byte[]> acceptedRecords,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        CommittedFence fence) {
        bindActorCommitted(
            acceptedRecords,
            targetNodeRid,
            targetNodeGeneration,
            fence.targetAuthorityOwnerGeneration());
        attachCanonicalFence(acceptedRecords, true, fence);
    }

    private void attachCanonicalFence(
        List<byte[]> acceptedRecords,
        boolean actor,
        CommittedFence fence) {
        Objects.requireNonNull(fence, "fence");
        for (byte[] acceptedRecord : acceptedRecords) {
            long operationHigh;
            long operationLow;
            boolean hasReply;
            if (actor) {
                var frozen = ZLinkServiceFrozenRecordCodec.decodeActor(
                    acceptedRecord);
                operationHigh = frozen.operationHigh();
                operationLow = frozen.operationLow();
                hasReply = frozen.replyRouteId().isPresent();
            } else {
                var frozen = ZLinkServiceFrozenRecordCodec.decodeSpot(
                    acceptedRecord);
                operationHigh = frozen.operationHigh();
                operationLow = frozen.operationLow();
                hasReply = frozen.replyRouteId().isPresent();
            }
            if (!hasReply) {
                continue;
            }
            Route route = routes.get(new OperationId(
                operationHigh, operationLow));
            if (route == null || !route.committed) {
                throw new IllegalStateException(
                    "canonical relocation reply capability is unavailable");
            }
            route.authorityKey = fence.authorityKey();
            route.participantId = fence.participantId();
        }
    }

    synchronized CanonicalRoute lookupCanonical(
        ZLinkServiceRelocationWireCodec.ReplyRelay relay,
        RoutingId transportSource) {
        Objects.requireNonNull(relay, "relay");
        Objects.requireNonNull(transportSource, "transportSource");
        removeExpired(Instant.now());
        Route route = routes.get(new OperationId(
            relay.operation().high(), relay.operation().low()));
        if (route == null || !route.committed
            || route.authorityKey == null || route.participantId == 0
            || route.replyRouteId != relay.replyRouteId()
            || route.participantId != relay.participantId()
            || route.targetAuthorityOwnerGeneration
                != relay.targetAttemptGeneration()
            || !transportSource.equals(route.targetNodeRid)
            || !transportSource.equals(relay.coordinator().nodeRid())
            || route.targetNodeGeneration
                != relay.coordinator().nodeGeneration()) {
            return null;
        }
        return new CanonicalRoute(
            new OperationId(relay.operation().high(), relay.operation().low()),
            route.authorityKey,
            route.spotId,
            route.objectGeneration,
            route.sourceOwnerId,
            route.sourceOwnerLeaseGeneration,
            route.sourceNodeRid,
            route.sourceNodeGeneration,
            route.replyRouteId,
            route.participantId);
    }

    CompletionStage<Ack> deliverCanonical(
        CanonicalRoute selected,
        List<byte[]> parts) {
        Objects.requireNonNull(selected, "selected");
        Objects.requireNonNull(parts, "parts");
        Route route;
        synchronized (this) {
            route = routes.get(selected.operation());
            if (route == null || !route.committed
                || !Objects.equals(
                    route.authorityKey, selected.authorityKey())
                || route.participantId != selected.participantId()) {
                return CompletableFuture.completedFuture(Ack.NOT_ACKNOWLEDGED);
            }
            if (route.delivered) {
                return CompletableFuture.completedFuture(Ack.ALREADY_TERMINAL);
            }
            if (route.relayInProgress) {
                return CompletableFuture.completedFuture(Ack.NOT_ACKNOWLEDGED);
            }
            route.relayInProgress = true;
        }
        CompletionStage<Void> delivery;
        try {
            delivery = Objects.requireNonNull(
                route.delivery.deliver(parts),
                "relocation reply delivery result");
        } catch (RuntimeException failure) {
            synchronized (this) { route.relayInProgress = false; }
            return CompletableFuture.failedFuture(failure);
        }
        return delivery.thenApply(ignored -> {
            synchronized (this) {
                route.relayInProgress = false;
                route.delivered = true;
            }
            return Ack.TERMINAL_RECEIVED;
        }).exceptionallyCompose(failure -> {
            synchronized (this) { route.relayInProgress = false; }
            return CompletableFuture.failedFuture(failure);
        });
    }

    CompletionStage<Ack> relay(
        Relay relay,
        RoutingId transportSource) {
        Objects.requireNonNull(relay, "relay");
        Objects.requireNonNull(transportSource, "transportSource");
        Route route;
        synchronized (this) {
            removeExpired(Instant.now());
            route = routes.get(relay.operation());
            if (route == null
                || !route.committed
                || !route.spotId.equals(relay.spotId())
                || route.objectGeneration != relay.objectGeneration()
                || !route.sourceOwnerId.equals(relay.sourceOwnerId())
                || route.sourceOwnerLeaseGeneration
                    != relay.sourceOwnerLeaseGeneration()
                || !route.sourceNodeRid.equals(relay.sourceNodeRid())
                || route.sourceNodeGeneration != relay.sourceNodeGeneration()
                || !transportSource.equals(route.targetNodeRid)
                || route.targetNodeGeneration != relay.targetNodeGeneration()
                || route.targetAuthorityOwnerGeneration
                    != relay.targetAuthorityOwnerGeneration()
                || route.replyRouteId != relay.replyRouteId()
                || relay.hopCount() < 0 || relay.hopCount() > 8) {
                return CompletableFuture.completedFuture(
                    Ack.NOT_ACKNOWLEDGED);
            }
            if (route.delivered) {
                return CompletableFuture.completedFuture(
                    Ack.ALREADY_TERMINAL);
            }
            if (route.relayInProgress) {
                return CompletableFuture.completedFuture(
                    Ack.NOT_ACKNOWLEDGED);
            }
            route.relayInProgress = true;
        }
        CompletionStage<Void> delivery;
        try {
            delivery = Objects.requireNonNull(
                route.delivery.deliver(relay.parts()),
                "relocation reply delivery result");
        } catch (RuntimeException failure) {
            synchronized (this) {
                route.relayInProgress = false;
            }
            return CompletableFuture.failedFuture(failure);
        }
        return delivery.thenApply(ignored -> {
            synchronized (this) {
                route.relayInProgress = false;
                route.delivered = true;
            }
            return Ack.TERMINAL_RECEIVED;
        }).exceptionallyCompose(failure -> {
            synchronized (this) {
                route.relayInProgress = false;
            }
            return CompletableFuture.failedFuture(failure);
        });
    }

    synchronized int size() {
        removeExpired(Instant.now());
        return routes.size();
    }

    private void removeExpired(Instant now) {
        routes.entrySet().removeIf(entry ->
            !entry.getValue().expiresAt.isAfter(now));
    }

    enum Ack {
        NOT_ACKNOWLEDGED,
        TERMINAL_RECEIVED,
        ALREADY_TERMINAL
    }

    record OperationId(long high, long low) {
        OperationId {
            if (high == 0 && low == 0) {
                throw new IllegalArgumentException(
                    "operation identity must not be zero");
            }
        }
    }

    record CommittedFence(
        String authorityKey,
        long participantId,
        long targetAuthorityOwnerGeneration) {
        CommittedFence {
            Objects.requireNonNull(authorityKey, "authorityKey");
            if (authorityKey.isBlank() || participantId == 0
                || targetAuthorityOwnerGeneration == 0) {
                throw new IllegalArgumentException(
                    "canonical committed reply fence is invalid");
            }
        }
    }

    record CanonicalRoute(
        OperationId operation,
        String authorityKey,
        String objectId,
        long objectGeneration,
        String sourceOwnerId,
        long sourceOwnerLeaseGeneration,
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        long replyRouteId,
        long participantId) {
    }

    record Relay(
        OperationId operation,
        long replyRouteId,
        String spotId,
        long objectGeneration,
        String sourceOwnerId,
        long sourceOwnerLeaseGeneration,
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        long targetNodeGeneration,
        long targetAuthorityOwnerGeneration,
        int hopCount,
        List<byte[]> parts) {
        Relay {
            Objects.requireNonNull(operation, "operation");
            Objects.requireNonNull(spotId, "spotId");
            Objects.requireNonNull(sourceOwnerId, "sourceOwnerId");
            Objects.requireNonNull(sourceNodeRid, "sourceNodeRid");
            parts = parts.stream().map(byte[]::clone).toList();
            if (replyRouteId <= 0 || objectGeneration <= 0
                || sourceOwnerLeaseGeneration <= 0
                || sourceNodeGeneration <= 0
                || targetNodeGeneration <= 0
                || targetAuthorityOwnerGeneration <= 0) {
                throw new IllegalArgumentException(
                    "reply relay fence contains a non-positive value");
            }
        }

        @Override public List<byte[]> parts() {
            return parts.stream().map(byte[]::clone).toList();
        }
    }

    static final class Registration {
        private final ZLinkSpotRelocationReplyRoutes owner;
        private final OperationId operation;
        private final Runnable relocationRelease;
        private final boolean request;
        private boolean released;

        private Registration(
            ZLinkSpotRelocationReplyRoutes owner,
            OperationId operation,
            Runnable relocationRelease,
            boolean request) {
            this.owner = owner;
            this.operation = operation;
            this.relocationRelease = relocationRelease;
            this.request = request;
        }

        synchronized void completeLocal() {
            if (released) {
                return;
            }
            released = true;
            if (request) {
                owner.completeLocal(operation);
            }
        }

        synchronized void releaseForRelocation() {
            if (released) {
                return;
            }
            released = true;
            // The transport receive can be released after its opaque reply
            // callback has been captured. The callback remains in Route until
            // terminal relay ACK or retention expiry.
            relocationRelease.run();
        }
    }

    private static final class Route {
        private final String spotId;
        private final long objectGeneration;
        private final String sourceOwnerId;
        private final long sourceOwnerLeaseGeneration;
        private final RoutingId sourceNodeRid;
        private final long sourceNodeGeneration;
        private final long replyRouteId;
        private final Instant expiresAt;
        private final Delivery delivery;
        private RoutingId targetNodeRid;
        private long targetNodeGeneration;
        private long targetAuthorityOwnerGeneration;
        private boolean committed;
        private boolean relayInProgress;
        private boolean delivered;
        private String authorityKey;
        private long participantId;

        private Route(
            String spotId,
            long objectGeneration,
            String sourceOwnerId,
            long sourceOwnerLeaseGeneration,
            RoutingId sourceNodeRid,
            long sourceNodeGeneration,
            long replyRouteId,
            Instant expiresAt,
            Delivery delivery) {
            this.spotId = spotId;
            this.objectGeneration = objectGeneration;
            this.sourceOwnerId = sourceOwnerId;
            this.sourceOwnerLeaseGeneration = sourceOwnerLeaseGeneration;
            this.sourceNodeRid = sourceNodeRid;
            this.sourceNodeGeneration = sourceNodeGeneration;
            this.replyRouteId = replyRouteId;
            this.expiresAt = expiresAt;
            this.delivery = delivery;
        }
    }

    @FunctionalInterface
    private interface Delivery {
        CompletionStage<Void> deliver(List<byte[]> parts);
    }
}

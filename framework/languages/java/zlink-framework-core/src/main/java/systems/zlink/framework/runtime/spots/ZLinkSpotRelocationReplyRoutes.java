package systems.zlink.framework.runtime.spots;
import java.util.ArrayList;

import java.time.Duration;
import java.time.Instant;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.Supplier;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
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
    private final ZLinkStateLane stateLane = new ZLinkStateLane();

    private <T> T inStateLane(Supplier<T> work) {
        try {
            return stateLane.runAsync(work).toCompletableFuture().join();
        } catch (CompletionException failure) {
            Throwable cause = failure.getCause();
            if (cause instanceof RuntimeException runtimeFailure) {
                throw runtimeFailure;
            }
            if (cause instanceof Error error) {
                throw error;
            }
            throw failure;
        }
    }

    private <T> CompletionStage<T> onStateLane(Supplier<T> work) {
        return stateLane.runAsync(work);
    }

    Registration register(
        byte[] acceptedRecord,
        ZLinkBackendReceived received,
        String spotId,
        long objectGeneration) {
        var frozen = ZLinkServiceFrozenRecordCodec.decodeSpot(acceptedRecord);
        OperationId operation = new OperationId(
            frozen.operationHigh(), frozen.operationLow());
        if (frozen.replyRouteId().isEmpty()) {
            return new Registration(this, operation, received::close, false);
        }
        RegistrationPlan plan = inStateLane(() -> registerOnLane(
            acceptedRecord, spotId, objectGeneration, received::close));
        Consumer<List<Message>> reply = received.reply();
        if (reply == null) {
            inStateLane(() -> {
                if (routes.get(plan.operation()) == plan.route()) {
                    routes.remove(plan.operation());
                }
                return null;
            });
            throw new IllegalStateException(
                "accepted request has no source reply capability");
        }
        inStateLane(() -> {
            if (routes.get(plan.operation()) == plan.route()) {
                plan.route().delivery = parts -> {
                    List<Message> messages = parts.stream().map(Message::from).toList();
                    try {
                        reply.accept(messages);
                        return CompletableFuture.completedFuture(null);
                    } catch (RuntimeException failure) {
                        messages.forEach(Message::close);
                        return CompletableFuture.failedFuture(failure);
                    }
                };
            }
            return null;
        });
        return plan.registration();
    }

    private RegistrationPlan registerOnLane(
        byte[] acceptedRecord, String spotId, long objectGeneration,
        Runnable relocationRelease) {
        var frozen = ZLinkServiceFrozenRecordCodec.decodeSpot(acceptedRecord);
        OperationId operation = new OperationId(
            frozen.operationHigh(), frozen.operationLow());
        removeExpired(Instant.now());
        if (routes.size() >= MAX_ROUTES) {
            throw new IllegalStateException(
                "Spot relocation reply-route registry is full");
        }
        Route route = new Route(
            spotId,
            objectGeneration,
            frozen.sourceOwnerId(),
            frozen.sourceOwnerLeaseGeneration(),
            frozen.sourceNodeRid(),
            frozen.sourceNodeGeneration(),
            frozen.replyRouteId().orElseThrow(),
            Instant.now().plus(RETENTION));
        Route previous = routes.putIfAbsent(operation, route);
        if (previous != null) {
            throw new IllegalStateException(
                "duplicate accepted relocation operation identity");
        }
        return new RegistrationPlan(operation, route,
            new Registration(this, operation, relocationRelease, true));
    }

    LazyRegistration registerLazy(
        Supplier<byte[]> acceptedRecord,
        ZLinkBackendReceived received,
        String spotId,
        long objectGeneration) {
        return new LazyRegistration(
            acceptedRecord,
            record -> register(record, received, spotId, objectGeneration));
    }

    Registration registerActor(
        byte[] acceptedRecord,
        String actorId,
        long objectGeneration,
        Function<List<byte[]>, CompletionStage<Void>> reply,
        Runnable relocationRelease) {
        return inStateLane(() -> registerActorOnLane(
            acceptedRecord, actorId, objectGeneration, reply, relocationRelease));
    }

    private Registration registerActorOnLane(
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
        Route route = new Route(
            actorId,
            objectGeneration,
            frozen.sourceOwnerId(),
            frozen.sourceOwnerLeaseGeneration(),
            frozen.sourceNodeRid(),
            frozen.sourceNodeGeneration(),
            frozen.replyRouteId().orElseThrow(),
            Instant.now().plus(RETENTION));
        route.delivery = parts -> reply.apply(
            parts.stream().map(byte[]::clone).toList());
        Route previous = routes.putIfAbsent(operation, route);
        if (previous != null) {
            throw new IllegalStateException(
                "duplicate accepted relocation operation identity");
        }
        return new Registration(
            this, operation, relocationRelease, true);
    }

    LazyRegistration registerActorLazy(
        Supplier<byte[]> acceptedRecord,
        String actorId,
        long objectGeneration,
        Function<List<byte[]>, CompletionStage<Void>> reply,
        Runnable relocationRelease) {
        return new LazyRegistration(
            acceptedRecord,
            record -> registerActor(
                record, actorId, objectGeneration, reply, relocationRelease));
    }

    void completeLocal(OperationId operation) {
        inStateLane(() -> {
            routes.remove(operation);
            return null;
        });
    }

    void bindCommitted(
        byte[] acceptedRecord,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        long targetAttemptGeneration) {
        bindCommitted(
            List.of(acceptedRecord),
            targetNodeRid,
            targetNodeGeneration,
            targetAttemptGeneration);
    }

    void bindCommitted(
        List<byte[]> acceptedRecords,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        CommittedFence fence) {
        inStateLane(() -> {
            bindCommittedOnLane(
            acceptedRecords,
            targetNodeRid,
            targetNodeGeneration,
            fence.targetAttemptGeneration());
            attachCanonicalFence(acceptedRecords, false, fence);
            return null;
        });
    }

    void bindCommitted(
        List<byte[]> acceptedRecords,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        long targetAttemptGeneration) {
        inStateLane(() -> {
            bindCommittedOnLane(acceptedRecords, targetNodeRid,
                targetNodeGeneration, targetAttemptGeneration);
            return null;
        });
    }

    private void bindCommittedOnLane(
        List<byte[]> acceptedRecords,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        long targetAttemptGeneration) {
        Objects.requireNonNull(acceptedRecords, "acceptedRecords");
        Objects.requireNonNull(targetNodeRid, "targetNodeRid");
        // targetNodeGeneration is a lifecycle-generation opaque equality
        // token (.NET ulong); a value with bit 63 set decodes to a negative
        // Java long, which is legitimate — only zero is unassigned. A signed
        // `<= 0` sentinel wrongly rejected it and flattened the failure to
        // STORE_UNAVAILABLE (spec 01-glossary lifecycle generation).
        if (targetNodeGeneration == 0
            || targetAttemptGeneration <= 0) {
            throw new IllegalArgumentException(
                "committed target reply fence requires a non-zero target node"
                    + " generation and a positive target attempt generation");
        }
        List<Route> selected = new ArrayList<>();
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
                    || route.targetAttemptGeneration
                        != targetAttemptGeneration)) {
                throw new IllegalStateException(
                    "committed relocation reply capability is unavailable");
            }
            selected.add(route);
        }
        for (Route route : selected) {
            route.targetNodeRid = targetNodeRid;
            route.targetNodeGeneration = targetNodeGeneration;
            route.targetAttemptGeneration = targetAttemptGeneration;
            route.committed = true;
        }
    }

    void bindActorCommitted(
        List<byte[]> acceptedRecords,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        long targetAttemptGeneration) {
        inStateLane(() -> {
            bindActorCommittedOnLane(acceptedRecords, targetNodeRid,
                targetNodeGeneration, targetAttemptGeneration);
            return null;
        });
    }

    private void bindActorCommittedOnLane(
        List<byte[]> acceptedRecords,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        long targetAttemptGeneration) {
        Objects.requireNonNull(acceptedRecords, "acceptedRecords");
        Objects.requireNonNull(targetNodeRid, "targetNodeRid");
        // targetNodeGeneration is a lifecycle-generation opaque equality
        // token (.NET ulong); a value with bit 63 set decodes to a negative
        // Java long, which is legitimate — only zero is unassigned. A signed
        // `<= 0` sentinel wrongly rejected it and flattened the failure to
        // STORE_UNAVAILABLE (spec 01-glossary lifecycle generation).
        if (targetNodeGeneration == 0
            || targetAttemptGeneration <= 0) {
            throw new IllegalArgumentException(
                "committed target reply fence requires a non-zero target node"
                    + " generation and a positive target attempt generation");
        }
        List<Route> selected = new ArrayList<>();
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
                    || route.targetAttemptGeneration
                        != targetAttemptGeneration)) {
                throw new IllegalStateException(
                    "committed Actor relocation reply capability is unavailable");
            }
            selected.add(route);
        }
        for (Route route : selected) {
            route.targetNodeRid = targetNodeRid;
            route.targetNodeGeneration = targetNodeGeneration;
            route.targetAttemptGeneration =
                targetAttemptGeneration;
            route.committed = true;
        }
    }

    void bindActorCommitted(
        List<byte[]> acceptedRecords,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        CommittedFence fence) {
        inStateLane(() -> {
            bindActorCommittedOnLane(
            acceptedRecords,
            targetNodeRid,
            targetNodeGeneration,
            fence.targetAttemptGeneration());
            attachCanonicalFence(acceptedRecords, true, fence);
            return null;
        });
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

    CanonicalRoute lookupCanonical(
        ZLinkServiceRelocationWireCodec.ReplyRelay relay,
        RoutingId transportSource) {
        return inStateLane(() -> lookupCanonicalOnLane(relay, transportSource));
    }

    private CanonicalRoute lookupCanonicalOnLane(
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
            || route.targetAttemptGeneration
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
        RouteSelection selection = inStateLane(() -> {
            Route route = routes.get(selected.operation());
            if (route == null || !route.committed
                || !Objects.equals(
                    route.authorityKey, selected.authorityKey())
                || route.participantId != selected.participantId()) {
                return RouteSelection.notAcknowledged();
            }
            if (route.delivered) {
                return RouteSelection.alreadyTerminal();
            }
            if (route.relayInProgress || route.delivery == null) {
                return RouteSelection.notAcknowledged();
            }
            route.relayInProgress = true;
            return RouteSelection.delivery(route);
        });
        if (selection.ack() != null) return CompletableFuture.completedFuture(selection.ack());
        Route route = selection.route();
        CompletionStage<Void> delivery;
        try {
            delivery = Objects.requireNonNull(
                route.delivery.deliver(parts),
                "relocation reply delivery result");
        } catch (RuntimeException failure) {
            inStateLane(() -> { route.relayInProgress = false; return null; });
            return CompletableFuture.failedFuture(failure);
        }
        return delivery.thenApply(ignored -> {
            inStateLane(() -> {
                route.relayInProgress = false;
                route.delivered = true;
                return null;
            });
            return Ack.TERMINAL_RECEIVED;
        }).exceptionallyCompose(failure -> {
            inStateLane(() -> { route.relayInProgress = false; return null; });
            return CompletableFuture.failedFuture(failure);
        });
    }

    CompletionStage<Ack> relay(
        Relay relay,
        RoutingId transportSource) {
        Objects.requireNonNull(relay, "relay");
        Objects.requireNonNull(transportSource, "transportSource");
        RouteSelection selection = inStateLane(() -> {
            removeExpired(Instant.now());
            Route route = routes.get(relay.operation());
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
                || route.targetAttemptGeneration
                    != relay.targetAttemptGeneration()
                || route.replyRouteId != relay.replyRouteId()
                || relay.hopCount() < 0 || relay.hopCount() > 8) {
                return RouteSelection.notAcknowledged();
            }
            if (route.delivered) {
                return RouteSelection.alreadyTerminal();
            }
            if (route.relayInProgress || route.delivery == null) {
                return RouteSelection.notAcknowledged();
            }
            route.relayInProgress = true;
            return RouteSelection.delivery(route);
        });
        if (selection.ack() != null) return CompletableFuture.completedFuture(selection.ack());
        Route route = selection.route();
        CompletionStage<Void> delivery;
        try {
            delivery = Objects.requireNonNull(
                route.delivery.deliver(relay.parts()),
                "relocation reply delivery result");
        } catch (RuntimeException failure) {
            inStateLane(() -> { route.relayInProgress = false; return null; });
            return CompletableFuture.failedFuture(failure);
        }
        return delivery.thenApply(ignored -> {
            inStateLane(() -> {
                route.relayInProgress = false;
                route.delivered = true;
                return null;
            });
            return Ack.TERMINAL_RECEIVED;
        }).exceptionallyCompose(failure -> {
            inStateLane(() -> { route.relayInProgress = false; return null; });
            return CompletableFuture.failedFuture(failure);
        });
    }

    int size() {
        return inStateLane(() -> {
            removeExpired(Instant.now());
            return routes.size();
        });
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
        long targetAttemptGeneration) {
        CommittedFence {
            Objects.requireNonNull(authorityKey, "authorityKey");
            if (authorityKey.isBlank() || participantId == 0
                || targetAttemptGeneration == 0) {
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
        long targetAttemptGeneration,
        int hopCount,
        List<byte[]> parts) {
        Relay {
            Objects.requireNonNull(operation, "operation");
            Objects.requireNonNull(spotId, "spotId");
            Objects.requireNonNull(sourceOwnerId, "sourceOwnerId");
            Objects.requireNonNull(sourceNodeRid, "sourceNodeRid");
            parts = parts.stream().map(byte[]::clone).toList();
            // sourceNodeGeneration/targetNodeGeneration are node
            // lifecycle-generation opaque equality tokens (.NET ulong, spec
            // 01-glossary "Lifecycle generation"): full range, only zero is
            // unassigned, so a signed `<= 0` sentinel wrongly rejects a
            // legitimate negative-as-long value (bit 63 set). The other
            // fields here (objectGeneration, sourceOwnerLeaseGeneration,
            // targetAttemptGeneration) are spec-bounded to
            // `1..long.MaxValue`, so `<= 0` is correct for them. ReplyRouteId
            // is separately a non-zero u64 identity (spec 51 §11).
            if (replyRouteId == 0 || objectGeneration <= 0
                || sourceOwnerLeaseGeneration <= 0
                || sourceNodeGeneration == 0
                || targetNodeGeneration == 0
                || targetAttemptGeneration <= 0) {
                throw new IllegalArgumentException(
                    "reply relay fence contains an invalid value");
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
        private final AtomicBoolean released = new AtomicBoolean();

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

        void completeLocal() {
            if (!released.compareAndSet(false, true)) return;
            if (request) {
                owner.completeLocal(operation);
            }
        }

        void releaseForRelocation() {
            if (!released.compareAndSet(false, true)) return;
            // The transport receive can be released after its opaque reply
            // callback has been captured. The callback remains in Route until
            // terminal relay ACK or retention expiry.
            relocationRelease.run();
        }
    }

    static final class LazyRegistration {
        private final Supplier<byte[]> recordSupplier;
        private final Function<byte[], Registration> registration;
        private final ZLinkStateLane stateLane = new ZLinkStateLane();
        private byte[] record;
        private Registration delegate;
        private boolean completedLocally;

        private LazyRegistration(
            Supplier<byte[]> recordSupplier,
            Function<byte[], Registration> registration) {
            this.recordSupplier = Objects.requireNonNull(
                recordSupplier, "recordSupplier");
            this.registration = Objects.requireNonNull(
                registration, "registration");
        }

        byte[] record() {
            byte[] existing = inStateLane(() -> {
                if (completedLocally) throw new IllegalStateException(
                    "completed dispatch cannot enter relocation");
                return record;
            });
            if (existing != null) return existing;
            byte[] supplied = Objects.requireNonNull(recordSupplier.get(),
                "accepted record supplier returned null");
            Registration created = registration.apply(supplied);
            return inStateLane(() -> {
                if (completedLocally) throw new IllegalStateException(
                    "completed dispatch cannot enter relocation");
                if (record == null) {
                    record = supplied;
                    delegate = created;
                }
                return record;
            });
        }

        void completeLocal() {
            Registration current = inStateLane(() -> {
                if (delegate == null) completedLocally = true;
                return delegate;
            });
            if (current != null) current.completeLocal();
        }

        void releaseForRelocation() {
            record();
            Registration current = inStateLane(() -> delegate);
            current.releaseForRelocation();
        }

        private <T> T inStateLane(Supplier<T> work) {
            try {
                return stateLane.runAsync(work).toCompletableFuture().join();
            } catch (CompletionException failure) {
                Throwable cause = failure.getCause();
                if (cause instanceof RuntimeException runtimeFailure) {
                    throw runtimeFailure;
                }
                if (cause instanceof Error error) throw error;
                throw failure;
            }
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
        private Delivery delivery;
        private RoutingId targetNodeRid;
        private long targetNodeGeneration;
        private long targetAttemptGeneration;
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
            Instant expiresAt) {
            this.spotId = spotId;
            this.objectGeneration = objectGeneration;
            this.sourceOwnerId = sourceOwnerId;
            this.sourceOwnerLeaseGeneration = sourceOwnerLeaseGeneration;
            this.sourceNodeRid = sourceNodeRid;
            this.sourceNodeGeneration = sourceNodeGeneration;
            this.replyRouteId = replyRouteId;
            this.expiresAt = expiresAt;
        }
    }

    private record RegistrationPlan(
        OperationId operation, Route route, Registration registration) { }

    private record RouteSelection(Route route, Ack ack) {
        private static RouteSelection delivery(Route route) {
            return new RouteSelection(route, null);
        }

        private static RouteSelection notAcknowledged() {
            return new RouteSelection(null, Ack.NOT_ACKNOWLEDGED);
        }

        private static RouteSelection alreadyTerminal() {
            return new RouteSelection(null, Ack.ALREADY_TERMINAL);
        }
    }

    @FunctionalInterface
    private interface Delivery {
        CompletionStage<Void> deliver(List<byte[]> parts);
    }
}

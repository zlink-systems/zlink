package systems.zlink.framework.runtime.spots;
import java.util.Arrays;
import java.util.Optional;

import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.ArrayList;
import java.util.List;
import java.util.function.Consumer;
import java.util.function.Supplier;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkRelocationAdapterRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

/**
 * Owns one independent Actor target attempt. Factory and Restore finish while
 * the Actor is absent from the live registry. Publication and admission are
 * separate so the caller can keep the target closed through durable replay.
 */
final class ZLinkStandaloneActorRelocationStagingOwner {
    private static final ZLinkRelocationCancellation NOT_CANCELLED =
        () -> false;

    private final Backend backend;

    ZLinkStandaloneActorRelocationStagingOwner(
        ZLinkInternalSpotNode targetNode,
        ZLinkActorSessionCoordinator actors,
        ZLinkRelocationAdapterRegistry adapters,
        ZLinkSpotRuntime spots) {
        backend = new ProductionBackend(
            Objects.requireNonNull(targetNode, "targetNode"),
            Objects.requireNonNull(actors, "actors"),
            Objects.requireNonNull(adapters, "adapters"),
            Objects.requireNonNull(spots, "spots"));
    }

    ZLinkStandaloneActorRelocationStagingOwner(Backend backend) {
        this.backend = Objects.requireNonNull(backend, "backend");
    }

    CompletionStage<Staged> stage(Request request, byte[] root) {
        Objects.requireNonNull(request, "request");
        var decoded = ZLinkCanonicalActorRelocationEnvelope.decode(
            root,
            request.relocationId(),
            request.actorId(),
            request.restoreSnapshot());
        if (decoded.objectGeneration() != request.objectGeneration()
            || decoded.expectedAuthorityOwnerGeneration()
                != request.sourceAuthorityOwnerGeneration()) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "Actor relocation root differs from the authority fence"));
        }
        return backend.prepare(request, decoded.state(), NOT_CANCELLED)
            .thenCompose(prepared -> backend.stageTimers(
                    request, decoded.timerEnvelope())
                .thenApply(ignored -> new Staged(
                    this, request, decoded, prepared)));
    }

    DurableBacklog closeDurableBacklog(
        Staged staged,
        byte[] finalRoot,
        ZLinkUserSpotAggregateStagingOwner.JournalReplayer replayer) {
        requireActive(staged);
        Objects.requireNonNull(replayer, "replayer");
        var decoded = ZLinkCanonicalActorRelocationEnvelope.decode(
            finalRoot,
            staged.request().relocationId(),
            staged.request().actorId(),
            staged.request().restoreSnapshot());
        requireStagingPrefix(staged, decoded);
        return inStateLane(staged, () -> {
            if (staged.ingressClosed) {
                throw new IllegalStateException(
                    "standalone Actor staging ingress was already closed");
            }
            staged.ingressClosed = true;
            staged.replaySealed = true;
            List<PendingIngress> relayed = List.copyOf(staged.relayedIngress);
            List<PendingIngress> temporary = List.copyOf(staged.pendingIngress);
            staged.relayedIngress.clear();
            staged.pendingIngress.clear();
            DurableBacklog backlog = new DurableBacklog(
                staged,
                decoded.journal(),
                relayed,
                temporary,
                replayer);
            staged.durableBacklog = backlog;
            return backlog;
        });
    }

    void publishHidden(
        DurableBacklog backlog,
        long targetOwnerGeneration) {
        Objects.requireNonNull(backlog, "backlog");
        Staged staged = backlog.staged;
        requireActive(staged);
        if (staged.durableBacklog != backlog
            || !staged.replaySealed
            || backlog.consumed) {
            throw new IllegalStateException(
                "Actor durable backlog publication fence is invalid");
        }
        if (targetOwnerGeneration < 0) {
            throw new IllegalArgumentException(
                "target owner generation must not be negative");
        }
        if (targetOwnerGeneration == 0) {
            publish(staged);
        } else {
            publish(staged, targetOwnerGeneration);
        }
    }

    CompletionStage<Optional<byte[]>> replayActor(
        Staged staged,
        ZLinkActorAcceptedJournal.Record record) {
        requireActive(staged);
        Objects.requireNonNull(record, "record");
        if (!record.actorId().equals(staged.request().actorId())) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "accepted journal references another Actor"));
        }
        return backend.replay(
            staged.actor(), staged.request(), record);
    }

    boolean acceptIngress(
        Staged staged,
        byte[] acceptedJournalRecord,
        Consumer<List<Message>> reply,
        Consumer<Throwable> failure) {
        requireActive(staged);
        return inStateLane(staged, () -> {
            if (staged.ingressClosed) {
                return false;
            }
            staged.pendingIngress.add(new PendingIngress(
                acceptedJournalRecord, reply, failure));
            return true;
        });
    }

    CompletionStage<Void> stageRelayedRecord(
        Staged staged,
        byte[] frozenRecord) {
        requireActive(staged);
        IllegalStateException closed = inStateLane(staged, () -> {
            if (staged.ingressClosed) {
                return new IllegalStateException(
                    "standalone Actor staging ingress is closed");
            }
            staged.relayedIngress.add(new PendingIngress(
                frozenRecord, null, null));
            return null;
        });
        if (closed != null) {
            return CompletableFuture.failedFuture(closed);
        }
        return CompletableFuture.completedFuture(null);
    }

    CompletionStage<Void> drainDurableBacklog(DurableBacklog backlog) {
        Objects.requireNonNull(backlog, "backlog");
        Staged staged = backlog.staged;
        requireActive(staged);
        if (!staged.published || !staged.lifecycleOpen) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "standalone Actor durable backlog is not runnable"));
        }
        IllegalStateException consumed = inStateLane(staged, () -> {
            if (backlog.consumed) {
                return new IllegalStateException(
                    "standalone Actor durable backlog was already consumed");
            }
            backlog.consumed = true;
            return null;
        });
        if (consumed != null) {
            return CompletableFuture.failedFuture(consumed);
        }
        CompletionStage<Void> replay = CompletableFuture.completedFuture(null);
        for (var queued : backlog.saved) {
            replay = replay.thenCompose(ignored -> admitBacklogTurn(
                () -> backlog.replayer.replay(
                    "actor:" + staged.request().actorId(), queued)));
        }
        for (PendingIngress ingress : backlog.relayed) {
            replay = replay.thenCompose(ignored -> admitBacklogTurn(
                () -> backlog.replayer.replayFrozen(
                    "actor:" + staged.request().actorId(), ingress.record())));
        }
        for (PendingIngress ingress : backlog.temporary) {
            replay = replay.thenCompose(ignored -> admitBacklogTurn(
                () -> replayIngress(staged, ingress)));
        }
        return replay.thenRun(() -> inStateLane(staged, () -> {
            staged.replayed = true;
            staged.durableBacklog = null;
            staged.terminal = true;
            return null;
        }));
    }

    private CompletionStage<Void> replayIngress(
        Staged staged,
        PendingIngress ingress) {
        return replayActor(
                staged,
                ZLinkActorAcceptedJournal.decode(ingress.record()))
            .handle((reply, failure) -> {
                if (failure != null) {
                    Throwable cause = failure instanceof
                        java.util.concurrent.CompletionException
                            && failure.getCause() != null
                        ? failure.getCause()
                        : failure;
                    if (ingress.failure() != null) {
                        ingress.failure().accept(cause);
                    }
                    throw new java.util.concurrent.CompletionException(cause);
                }
                if (reply.isPresent() && ingress.reply() != null) {
                    List<Message> parts = List.of(
                        Message.from(reply.orElseThrow()));
                    try {
                        ingress.reply().accept(parts);
                    } catch (RuntimeException callbackFailure) {
                        parts.forEach(Message::close);
                        throw callbackFailure;
                    }
                }
                return null;
            });
    }

    private CompletionStage<Void> admitBacklogTurn(
        Supplier<CompletionStage<Void>> turn) {
        try {
            return backend.admitApplicationJob(turn);
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    DirectJoinReplay closeDirectJoinIngress(
        Staged staged,
        byte[] finalRoot) {
        requireActive(staged);
        var decoded = ZLinkCanonicalActorRelocationEnvelope.decode(
            finalRoot,
            staged.request().relocationId(),
            staged.request().actorId(),
            staged.request().restoreSnapshot());
        requireStagingPrefix(staged, decoded);
        return inStateLane(staged, () -> {
            if (staged.ingressClosed) {
                throw new IllegalStateException(
                    "standalone Actor staging ingress was already closed");
            }
            staged.ingressClosed = true;
            staged.directReplaySealed = true;
            List<PendingIngress> relayed = List.copyOf(staged.relayedIngress);
            List<PendingIngress> temporary = List.copyOf(staged.pendingIngress);
            staged.relayedIngress.clear();
            staged.pendingIngress.clear();
            DirectJoinReplay replay = new DirectJoinReplay(
                staged,
                decoded.journal(),
                relayed,
                temporary);
            staged.directBacklog = replay;
            return replay;
        });
    }

    void publishDirectJoinHidden(
        DirectJoinReplay replay,
        long targetOwnerGeneration) {
        Objects.requireNonNull(replay, "replay");
        Staged staged = replay.staged;
        requireActive(staged);
        if (!staged.directReplaySealed
            || staged.directBacklog != replay
            || replay.replayed) {
            throw new IllegalStateException(
                "direct Join durable backlog publication fence is invalid");
        }
        backend.publish(
            staged.actor(), staged.request(), targetOwnerGeneration);
        staged.published = true;
    }

    void prepareDirectJoinBoundSession(
        DirectJoinReplay replay,
        ZLinkSpotRetireControl.StageRequest request,
        long targetOwnerGeneration) {
        Objects.requireNonNull(replay, "replay");
        Objects.requireNonNull(request, "request");
        if (request.sessionRoutes().isEmpty()) {
            return;
        }
        Staged staged = replay.staged();
        requireActive(staged);
        if (!staged.published || staged.directBacklog != replay) {
            throw new IllegalStateException(
                "direct Join bound Session requires hidden publication");
        }
        ZLinkSpotRetireControl.SessionRouteFence route =
            request.sessionRoutes().getFirst();
        if (!route.actorId().equals(staged.request().actorId())
            || route.actorObjectGeneration()
                != staged.request().objectGeneration()) {
            throw new IllegalStateException(
                "direct Join bound Session differs from the staged Actor");
        }
        backend.prepareBoundSession(
            staged.actor(),
            staged.request(),
            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                new ZLinkBackendActorRef(
                    request.targetNodeRid(),
                    route.actorId(),
                    route.actorObjectGeneration()),
                request.targetNodeGeneration(),
                targetOwnerGeneration,
                request.targetOwnerLeaseGeneration()),
            new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                route.sessionOwnerNodeRid(),
                route.sessionOwnerNodeGeneration(),
                route.sessionOwnerId(),
                route.sessionOwnerLeaseGeneration(),
                route.sessionRid(),
                route.bindingGeneration()));
    }

    CompletionStage<Void> replayDirectJoin(
        DirectJoinReplay replay,
        ZLinkUserSpotAggregateStagingOwner.JournalReplayer replayer) {
        Objects.requireNonNull(replay, "replay");
        Objects.requireNonNull(replayer, "replayer");
        Staged staged = replay.staged();
        requireActive(staged);
        if (!staged.published || !staged.lifecycleOpen) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "direct Join durable backlog is not runnable"));
        }
        IllegalStateException alreadyReplayed = inStateLane(staged, () -> {
            if (replay.replayed) {
                return new IllegalStateException(
                    "direct Join replay was already consumed");
            }
            replay.replayed = true;
            return null;
        });
        if (alreadyReplayed != null) {
            return CompletableFuture.failedFuture(alreadyReplayed);
        }
        CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
        for (var queued : replay.saved()) {
            chain = chain.thenCompose(ignored -> admitBacklogTurn(
                () -> replayer.replay(
                    "actor:" + staged.request().actorId(), queued)));
        }
        for (PendingIngress ingress : replay.relayed()) {
            chain = chain.thenCompose(ignored -> admitBacklogTurn(
                () -> replayer.replayFrozen(
                    "actor:" + staged.request().actorId(), ingress.record())));
        }
        for (PendingIngress ingress : replay.temporary()) {
            chain = chain.thenCompose(ignored -> admitBacklogTurn(
                () -> replayIngress(staged, ingress)));
        }
        return chain.thenRun(() -> inStateLane(staged, () -> {
            staged.replayed = true;
            staged.directBacklog = null;
            staged.terminal = true;
            return null;
        }));
    }

    private static void requireStagingPrefix(
        Staged staged,
        ZLinkCanonicalActorRelocationEnvelope.Decoded decoded) {
        var initial = staged.decoded();
        if (decoded.objectGeneration() != initial.objectGeneration()
            || decoded.expectedAuthorityOwnerGeneration()
                != initial.expectedAuthorityOwnerGeneration()
            || !Arrays.equals(decoded.state(), initial.state())) {
            throw new IllegalArgumentException(
                "authority-selected Actor root differs from staged state");
        }
        if (decoded.journal().size() < initial.journal().size()) {
            throw new IllegalArgumentException(
                "authority-selected Actor journal removed staged records");
        }
        for (int index = 0; index < initial.journal().size(); index++) {
            var left = initial.journal().get(index);
            var right = decoded.journal().get(index);
            if (left.sequence() != right.sequence()
                || !Arrays.equals(left.payload(), right.payload())) {
                throw new IllegalArgumentException(
                    "authority-selected Actor journal changed its prefix");
            }
        }
    }

    private void publish(Staged staged) {
        requireActive(staged);
        backend.publish(staged.actor(), staged.request());
        staged.published = true;
    }

    private void publish(Staged staged, long targetOwnerGeneration) {
        requireActive(staged);
        backend.publish(
            staged.actor(),
            staged.request(),
            targetOwnerGeneration);
        staged.published = true;
    }

    void openAdmission(Staged staged) {
        requireActive(staged);
        if (!staged.published) {
            throw new IllegalStateException(
                "Actor relocation is not published");
        }
        if (staged.lifecycleOpen) {
            throw new IllegalStateException(
                "Actor relocation admission is already open");
        }
        backend.publishTimers(staged.request());
        backend.openAdmission(staged.actor());
        staged.lifecycleOpen = true;
    }

    CompletionStage<Void> discard(Staged staged) {
        Objects.requireNonNull(staged, "staged");
        if (staged.owner != this) {
            throw new IllegalStateException(
                "Actor relocation target attempt belongs to another owner");
        }
        List<PendingIngress> pending = inStateLane(staged, () -> {
            if (staged.published) {
                throw new IllegalStateException(
                    "published Actor relocation cannot be discarded");
            }
            if (staged.terminal) {
                return null;
            }
            staged.ingressClosed = true;
            staged.terminal = true;
            List<PendingIngress> captured = new ArrayList<>(staged.relayedIngress.size()
                + staged.pendingIngress.size());
            captured.addAll(staged.relayedIngress);
            captured.addAll(staged.pendingIngress);
            if (staged.durableBacklog != null) {
                captured.addAll(staged.durableBacklog.relayed);
                captured.addAll(staged.durableBacklog.temporary);
                staged.durableBacklog = null;
            }
            if (staged.directBacklog != null) {
                captured.addAll(staged.directBacklog.relayed);
                captured.addAll(staged.directBacklog.temporary);
                staged.directBacklog = null;
            }
            staged.relayedIngress.clear();
            staged.pendingIngress.clear();
            return captured;
        });
        if (pending == null) {
            return CompletableFuture.completedFuture(null);
        }
        IllegalStateException aborted = new IllegalStateException(
            "Actor relocation target staging was aborted");
        for (PendingIngress ingress : pending) {
            if (ingress.failure() != null) {
                ingress.failure().accept(aborted);
            }
        }
        return backend.discard(staged.actor(), staged.request());
    }

    private void requireActive(Staged staged) {
        Objects.requireNonNull(staged, "staged");
        if (staged.owner != this || staged.terminal) {
            throw new IllegalStateException(
                "Actor relocation target attempt fence is not active");
        }
    }

    private static <T> T inStateLane(Staged staged, Supplier<T> work) {
        try {
            return staged.stateLane.runAsync(work).toCompletableFuture().join();
        } catch (java.util.concurrent.CompletionException failure) {
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

    record Request(
        UUID relocationId,
        String actorId,
        String stableType,
        long objectGeneration,
        long sourceAuthorityOwnerGeneration,
        boolean restoreSnapshot,
        String targetSpotId) {
        Request {
            Objects.requireNonNull(relocationId, "relocationId");
            requireText(actorId, "actorId");
            requireText(stableType, "stableType");
            requireText(targetSpotId, "targetSpotId");
            if (relocationId.equals(new UUID(0, 0))
                || objectGeneration <= 0
                || sourceAuthorityOwnerGeneration <= 0) {
                throw new IllegalArgumentException(
                    "Actor relocation identity and generations must be positive");
            }
        }
    }

    static final class Staged {
        private final ZLinkStandaloneActorRelocationStagingOwner owner;
        private final Request request;
        private final ZLinkCanonicalActorRelocationEnvelope.Decoded decoded;
        private final Object actor;
        private final List<PendingIngress> relayedIngress = new ArrayList<>();
        private final List<PendingIngress> pendingIngress = new ArrayList<>();
        private final ZLinkStateLane stateLane = new ZLinkStateLane();
        private DurableBacklog durableBacklog;
        private DirectJoinReplay directBacklog;
        private boolean replayed;
        private boolean published;
        private boolean ingressClosed;
        private boolean replaySealed;
        private boolean directReplaySealed;
        private boolean lifecycleOpen;
        private boolean terminal;

        private Staged(
            ZLinkStandaloneActorRelocationStagingOwner owner,
            Request request,
            ZLinkCanonicalActorRelocationEnvelope.Decoded decoded,
            Object actor) {
            this.owner = owner;
            this.request = request;
            this.decoded = decoded;
            this.actor = actor;
        }

        Request request() {
            return request;
        }

        ZLinkCanonicalActorRelocationEnvelope.Decoded decoded() {
            return decoded;
        }

        Object actor() {
            return actor;
        }
    }

    static final class DurableBacklog {
        private final Staged staged;
        private final List<ZLinkAsyncSerialQueue.QueuedRecord> saved;
        private final List<PendingIngress> relayed;
        private final List<PendingIngress> temporary;
        private final ZLinkUserSpotAggregateStagingOwner.JournalReplayer replayer;
        private boolean consumed;

        private DurableBacklog(
            Staged staged,
            List<ZLinkAsyncSerialQueue.QueuedRecord> saved,
            List<PendingIngress> relayed,
            List<PendingIngress> temporary,
            ZLinkUserSpotAggregateStagingOwner.JournalReplayer replayer) {
            this.staged = staged;
            this.saved = List.copyOf(saved);
            this.relayed = List.copyOf(relayed);
            this.temporary = List.copyOf(temporary);
            this.replayer = replayer;
        }
    }

    static final class DirectJoinReplay {
        private final Staged staged;
        private final List<ZLinkAsyncSerialQueue.QueuedRecord> saved;
        private final List<PendingIngress> relayed;
        private final List<PendingIngress> temporary;
        private boolean replayed;

        private DirectJoinReplay(
            Staged staged,
            List<ZLinkAsyncSerialQueue.QueuedRecord> saved,
            List<PendingIngress> relayed,
            List<PendingIngress> temporary) {
            this.staged = staged;
            this.saved = List.copyOf(saved);
            this.relayed = List.copyOf(relayed);
            this.temporary = List.copyOf(temporary);
        }

        private Staged staged() {
            return staged;
        }

        private List<ZLinkAsyncSerialQueue.QueuedRecord> saved() {
            return saved;
        }

        private List<PendingIngress> relayed() {
            return relayed;
        }

        private List<PendingIngress> temporary() {
            return temporary;
        }
    }

    private record PendingIngress(
        byte[] record,
        Consumer<List<Message>> reply,
        Consumer<Throwable> failure) {
        private PendingIngress {
            record = Objects.requireNonNull(record, "record").clone();
        }

        @Override public byte[] record() {
            return record.clone();
        }
    }

    interface Backend {
        CompletionStage<Object> prepare(
            Request request,
            byte[] state,
            ZLinkRelocationCancellation cancellation);

        CompletionStage<Optional<byte[]>> replay(
            Object actor,
            Request request,
            ZLinkActorAcceptedJournal.Record record);

        default <T> CompletionStage<T> admitApplicationJob(
            Supplier<CompletionStage<T>> turn) {
            return Objects.requireNonNull(turn, "turn").get();
        }

        default CompletionStage<Void> stageTimers(
            Request request,
            byte[] timerEnvelope) {
            if (ZLinkSpotTimerRelocationEnvelope
                    .canonicalize(timerEnvelope).isEmpty()) {
                return CompletableFuture.completedFuture(null);
            }
            return CompletableFuture.failedFuture(new IllegalStateException(
                "standalone Actor timer staging is unavailable"));
        }

        default void publish(Object actor, Request request) {
            throw new IllegalStateException(
                "Actor publication requires a target owner generation");
        }

        default void publish(
            Object actor,
            Request request,
            long targetOwnerGeneration) {
            if (targetOwnerGeneration <= 0) {
                throw new IllegalArgumentException(
                    "target owner generation must be positive");
            }
            publish(actor, request);
        }

        default void prepareBoundSession(
            Object actor,
            Request request,
            ZLinkServiceM6BWireCodec.ActorRouteFence actorRoute,
            ZLinkServiceM6BWireCodec.SessionOwnerFence session) {
        }

        void openAdmission(Object actor);

        default void publishTimers(Request request) {
        }

        CompletionStage<Void> discard(Object actor, Request request);
    }

    private static final class ProductionBackend implements Backend {
        private final ZLinkInternalSpotNode targetNode;
        private final ZLinkActorSessionCoordinator actors;
        private final ZLinkRelocationAdapterRegistry adapters;
        private final ZLinkSpotRuntime spots;

        private ProductionBackend(
            ZLinkInternalSpotNode targetNode,
            ZLinkActorSessionCoordinator actors,
            ZLinkRelocationAdapterRegistry adapters,
            ZLinkSpotRuntime spots) {
            this.targetNode = targetNode;
            this.actors = actors;
            this.adapters = adapters;
            this.spots = spots;
        }

        @Override
        public CompletionStage<Object> prepare(
            Request request,
            byte[] state,
            ZLinkRelocationCancellation cancellation) {
            return actors.prepareRelocatedActor(
                    request.actorId(),
                    request.stableType(),
                    state,
                    request.restoreSnapshot(),
                    adapters,
                    cancellation,
                    new ZLinkBackendActorRef(
                        targetNode.routingId(),
                        request.actorId(),
                        request.objectGeneration()))
                .thenApply(value -> value);
        }

        @Override
        public CompletionStage<Optional<byte[]>> replay(
            Object actor,
            Request request,
            ZLinkActorAcceptedJournal.Record record) {
            return spots.replayPreparedActorAtSpot(
                request.targetSpotId(),
                (ZLinkActorRuntime.PreparedTransferredActor) actor,
                record);
        }

        @Override
        public <T> CompletionStage<T> admitApplicationJob(
            Supplier<CompletionStage<T>> turn) {
            return spots.admitNewApplicationJob(turn);
        }

        @Override
        public CompletionStage<Void> stageTimers(
            Request request,
            byte[] timerEnvelope) {
            spots.stageEntryActorTimerRelocationEnvelope(
                request.targetSpotId(),
                request.actorId(),
                timerEnvelope);
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public void publish(Object actor, Request request) {
            throw new IllegalStateException(
                "standalone Actor publication requires the committed owner generation");
        }

        @Override
        public void publish(
            Object actor,
            Request request,
            long targetOwnerGeneration) {
            if (targetOwnerGeneration <= 0) {
                throw new IllegalArgumentException(
                    "target owner generation must be positive");
            }
            actors.publishRelocatedActor(
                (ZLinkActorRuntime.PreparedTransferredActor) actor,
                request.targetSpotId(),
                targetOwnerGeneration);
        }

        @Override
        public void prepareBoundSession(
            Object actor,
            Request request,
            ZLinkServiceM6BWireCodec.ActorRouteFence actorRoute,
            ZLinkServiceM6BWireCodec.SessionOwnerFence session) {
            var prepared = (ZLinkActorRuntime.PreparedTransferredActor) actor;
            ZLinkActor targetActor = prepared.actor();
            targetNode.installRelocatingActorBoundSession(actorRoute, session);
            actors.bindNativeSession(
                targetActor,
                targetNode,
                actorRoute.actor(),
                session.nodeRid(),
                session.sessionRid());
        }

        @Override
        public void openAdmission(Object actor) {
            actors.openRelocatedActorAdmission(
                (ZLinkActorRuntime.PreparedTransferredActor) actor);
        }

        @Override
        public void publishTimers(Request request) {
            spots.publishEntryActorTimerRelocation(
                request.targetSpotId(),
                request.actorId());
        }

        @Override
        public CompletionStage<Void> discard(Object actor, Request request) {
            var prepared =
                (ZLinkActorRuntime.PreparedTransferredActor) actor;
            String actorId = prepared.actor().context().actorId();
            return actors.discardRelocatedActor(prepared)
                .thenRun(() -> spots.discardEntryActorTimerRelocation(
                    request.targetSpotId(), actorId));
        }
    }

    private static String requireText(String value, String name) {
        if (value == null || value.isBlank() || value.indexOf('\0') >= 0) {
            throw new IllegalArgumentException(name + " is invalid");
        }
        return value;
    }
}

package systems.zlink.framework.runtime.spots;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Optional;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;
import java.util.function.Supplier;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkRelocationAdapterRegistry;
import systems.zlink.framework.spots.ZLinkSpot;

/**
 * Owns target factory and Restore staging for one User Spot aggregate.
 * Prepared objects stay outside live registries until every participant,
 * timer and accepted-journal record has been validated.
 */
final class ZLinkUserSpotAggregateStagingOwner {
    private final StagingBackend backend;

    ZLinkUserSpotAggregateStagingOwner(
        ZLinkSpotLifecycle spots,
        ZLinkActorSessionCoordinator actorSessions,
        ZLinkRelocationAdapterRegistry adapters) {
        backend = new ProductionBackend(
            Objects.requireNonNull(spots, "spots"),
            Objects.requireNonNull(actorSessions, "actorSessions").runtime(),
            Objects.requireNonNull(adapters, "adapters"));
    }

    ZLinkUserSpotAggregateStagingOwner(
        ZLinkSpotRuntime spots,
        ZLinkRelocationAdapterRegistry adapters) {
        Objects.requireNonNull(spots, "spots");
        backend = new ProductionBackend(
            spots.spotLifecycle(),
            spots.actorSessions().runtime(),
            Objects.requireNonNull(adapters, "adapters"),
            spots);
    }

    ZLinkUserSpotAggregateStagingOwner(StagingBackend backend) {
        this.backend = Objects.requireNonNull(backend, "backend");
    }

    CompletionStage<Staged> stage(
        Request request,
        ZLinkRelocationCancellation cancellation) {
        Objects.requireNonNull(request, "request");
        Objects.requireNonNull(cancellation, "cancellation");
        List<Object> preparedActors = new ArrayList<>();
        return prepareAndRestoreSpot(request, cancellation)
            .thenCompose(preparedSpot -> prepareActors(
                    preparedSpot,
                    request,
                    cancellation,
                    preparedActors)
                .thenApply(ignored -> new Staged(
                        this,
                        request,
                        preparedSpot,
                        preparedActors,
                        backend.beginIngressHold(preparedSpot)))
                .exceptionallyCompose(failure -> discardPartial(
                    preparedSpot,
                    preparedActors)
                    .thenCompose(ignored -> CompletableFuture.failedFuture(
                        unwrap(failure)))));
    }

    /**
     * Prepares a fresh Spot instance and restores it. A restore failure
     * discards the partially-restored instance and fails explicitly — no
     * retry, no partial reuse. Capture/factory/restore/staging failures are
     * {@link ZLinkFrameworkErrorKind#INTERNAL_FAILURE} per the spec 15
     * failure table; {@code DATA_LOST} stays reserved for chunk-assembly
     * or checksum verification failures and is never produced here — it is
     * only preserved when the cause already carries that classification
     * (spec 07e0234db5).
     */
    private CompletionStage<Object> prepareAndRestoreSpot(
        Request request,
        ZLinkRelocationCancellation cancellation) {
        return backend.prepareSpot(request)
            .thenCompose(preparedSpot -> backend.restoreSpot(
                    preparedSpot,
                    request,
                    cancellation)
                .thenApply(ignored -> preparedSpot)
                .exceptionallyCompose(failure -> {
                    backend.discardSpot(preparedSpot);
                    Throwable cause = unwrap(failure);
                    return CompletableFuture.failedFuture(
                        relocationInternalFailure(cause));
                }));
    }

    private static ZLinkFrameworkException relocationInternalFailure(
        Throwable cause) {
        if (cause instanceof ZLinkFrameworkException framework) {
            //  The deeper layer already classified this failure (e.g. a
            //  genuine chunk-assembly/checksum DATA_LOST) — preserve it
            //  rather than relabeling it as a generic internal failure.
            return framework;
        }
        return new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
            "User Spot relocation restore failed; the target instance is "
                + "discarded, not partially reused",
            cause);
    }

    private CompletionStage<Void> prepareActors(
        Object preparedSpot,
        Request request,
        ZLinkRelocationCancellation cancellation,
        List<Object> prepared) {
        CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
        for (ActorParticipant participant : request.actors()) {
            chain = chain.thenCompose(ignored -> backend.prepareActor(
                    participant,
                    cancellation)
                .thenCompose(actor -> {
                    prepared.add(actor);
                    return backend.stageActorTimers(
                        preparedSpot,
                        actor,
                        participant,
                        participant.timerEnvelope());
                }));
        }
        return chain;
    }

    private CompletionStage<Void> discardPartial(
        Object spot,
        List<Object> actorsToDiscard) {
        CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
        for (int index = actorsToDiscard.size() - 1; index >= 0; index--) {
            var actor = actorsToDiscard.get(index);
            chain = chain.thenCompose(ignored ->
                backend.discardActor(actor));
        }
        return chain.whenComplete((ignored, failure) -> backend.discardSpot(spot));
    }

    CompletionStage<Void> publishAndReplay(
        Staged staged,
        JournalReplayer replayer) {
        return publishAndReplay(staged, staged.request, replayer);
    }

    CompletionStage<Void> publishAndReplay(
        Staged staged,
        Request finalRequest,
        JournalReplayer replayer) {
        return closeDurableBacklog(staged, finalRequest, replayer)
            .thenCompose(backlog -> {
                publishHidden(backlog, Map.of());
                openAdmission(staged);
                return drainDurableBacklog(backlog);
            });
    }

    CompletionStage<DurableBacklog> closeDurableBacklog(
        Staged staged,
        Request finalRequest,
        JournalReplayer replayer) {
        requireActive(staged);
        requireStagingPrefix(staged.request, finalRequest);
        Objects.requireNonNull(replayer, "replayer");
        List<PendingIngress> relayed;
        List<PendingIngress> temporary;
        DurableBacklog backlog;
        synchronized (staged) {
            if (staged.ingressClosed) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "aggregate staging ingress was already closed"));
            }
            staged.ingressClosed = true;
            relayed = List.copyOf(staged.relayedIngress);
            temporary = List.copyOf(staged.pendingIngress);
            staged.relayedIngress.clear();
            staged.pendingIngress.clear();
            backlog = new DurableBacklog(
                this,
                staged,
                finalRequest,
                replayer,
                relayed,
                temporary);
            staged.durableBacklog = backlog;
        }
        return backend.completeRelocationReady(staged.spot)
            .thenApply(ignored -> {
                staged.backlogSealed = true;
                return backlog;
            });
    }

    void publishHidden(
        DurableBacklog backlog,
        Map<String, Long> actorOwnerGenerations) {
        Objects.requireNonNull(backlog, "backlog");
        if (backlog.owner != this) {
            throw new IllegalStateException(
                "aggregate durable backlog belongs to another owner");
        }
        Staged staged = backlog.staged;
        requireActive(staged);
        if (!staged.backlogSealed
            || staged.durableBacklog != backlog
            || backlog.consumed) {
            throw new IllegalStateException(
                "aggregate durable backlog publication fence is invalid");
        }
        Objects.requireNonNull(actorOwnerGenerations, "actorOwnerGenerations");
        for (int index = 0; index < staged.actors.size(); index++) {
            Object actor = staged.actors.get(index);
            String actorId = backlog.finalRequest.actors().get(index).actorId();
            Long ownerGeneration = actorOwnerGenerations.get(actorId);
            if (ownerGeneration == null) {
                backend.publishActor(actor);
            } else {
                backend.publishActor(
                    actor,
                    backlog.finalRequest.spotId(),
                    ownerGeneration);
            }
        }
        backend.publishSpot(staged.spot);
        staged.published = true;
    }

    void openAdmission(Staged staged) {
        requireActive(staged);
        if (!staged.published) {
            throw new IllegalStateException(
                "aggregate staging is not published");
        }
        if (!staged.backlogSealed || staged.lifecycleOpen) {
            throw new IllegalStateException(
                "aggregate durable backlog or lifecycle fence is invalid");
        }
        for (int index = 0; index < staged.actors.size(); index++) {
            Object actor = staged.actors.get(index);
            ActorParticipant participant = staged.request.actors().get(index);
            backend.completeActor(actor);
            backend.publishActorTimers(
                staged.spot, actor, participant);
        }
        backend.publishTimers(staged.spot);
        staged.lifecycleOpen = true;
    }

    boolean acceptSpotIngress(
        Staged staged,
        byte[] acceptedJournalRecord,
        Consumer<List<Message>> reply,
        Consumer<Throwable> failure) {
        requireActive(staged);
        synchronized (staged) {
            if (staged.ingressClosed) {
                return false;
            }
            staged.pendingIngress.add(new PendingIngress(
                "spot",
                Objects.requireNonNull(
                    acceptedJournalRecord,
                    "acceptedJournalRecord"),
                reply,
                failure));
            return true;
        }
    }

    boolean acceptActorIngress(
        Staged staged,
        String actorId,
        byte[] acceptedJournalRecord,
        Consumer<List<Message>> reply,
        Consumer<Throwable> failure) {
        requireActive(staged);
        if (staged.request.actors().stream().noneMatch(
            participant -> participant.actorId().equals(actorId))) {
            return false;
        }
        synchronized (staged) {
            if (staged.ingressClosed) {
                return false;
            }
            staged.pendingIngress.add(new PendingIngress(
                actorId,
                Objects.requireNonNull(
                    acceptedJournalRecord,
                    "acceptedJournalRecord"),
                reply,
                failure));
            return true;
        }
    }

    CompletionStage<Void> stageRelayedRecord(
        Staged staged,
        String objectId,
        boolean actor,
        byte[] frozenRecord) {
        requireActive(staged);
        if (actor && staged.request.actors().stream().noneMatch(
                participant -> participant.actorId().equals(objectId))) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "relayed record Actor is outside the staged aggregate"));
        }
        synchronized (staged) {
            if (staged.ingressClosed) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "aggregate staging ingress is closed"));
            }
            staged.relayedIngress.add(new PendingIngress(
                actor ? objectId : "spot",
                Objects.requireNonNull(frozenRecord, "frozenRecord"),
                null,
                null));
        }
        return CompletableFuture.completedFuture(null);
    }

    CompletionStage<Void> drainDurableBacklog(DurableBacklog backlog) {
        Objects.requireNonNull(backlog, "backlog");
        if (backlog.owner != this) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "aggregate durable backlog belongs to another owner"));
        }
        Staged staged = backlog.staged;
        requireActive(staged);
        if (!staged.published || !staged.lifecycleOpen) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "aggregate durable backlog is not runnable"));
        }
        synchronized (backlog) {
            if (backlog.consumed) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "aggregate durable backlog was already consumed"));
            }
            backlog.consumed = true;
        }
        CompletionStage<Void> replay = CompletableFuture.completedFuture(null);
        for (Map.Entry<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> lane
            : backlog.finalRequest.acceptedJournal().entrySet()) {
            for (ZLinkAsyncSerialQueue.QueuedRecord record : lane.getValue()) {
                replay = replay.thenCompose(ignored -> admitBacklogTurn(
                    () -> backlog.replayer.replay(lane.getKey(), record)));
            }
        }
        for (PendingIngress ingress : backlog.relayed) {
            replay = replay.thenCompose(ignored -> admitBacklogTurn(
                () -> backlog.replayer.replayFrozen(
                    ingress.laneId(), ingress.record())));
        }
        for (PendingIngress ingress : backlog.temporary) {
            replay = replay.thenCompose(ignored -> admitBacklogTurn(
                () -> replayIngress(staged, ingress)));
        }
        return replay.thenRun(() -> {
            backend.resumeIngress(staged.spot, staged.ingressHold);
            staged.durableBacklog = null;
            staged.terminal = true;
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

    private CompletionStage<Void> replayIngress(
        Staged staged,
        PendingIngress ingress) {
        CompletionStage<List<byte[]>> replay;
        try {
            if (ingress.laneId().equals("spot")) {
                replay = replaySpot(
                    staged,
                    ZLinkSpotAcceptedJournal.decode(ingress.record()));
            } else {
                replay = replayActor(
                        staged,
                        ZLinkActorAcceptedJournal.decode(ingress.record()))
                    .thenApply(reply -> reply.stream().toList());
            }
        } catch (RuntimeException failure) {
            notifyIngressFailure(ingress, failure);
            return CompletableFuture.failedFuture(failure);
        }
        return replay.handle((reply, failure) -> {
            if (failure != null) {
                Throwable cause = unwrap(failure);
                notifyIngressFailure(ingress, cause);
                throw new CompletionException(cause);
            }
            if (ingress.reply() != null && !reply.isEmpty()) {
                List<Message> parts = reply.stream()
                    .map(Message::from)
                    .toList();
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

    private static void notifyIngressFailure(
        PendingIngress ingress,
        Throwable failure) {
        if (ingress.failure() != null) {
            ingress.failure().accept(failure);
        }
    }

    CompletionStage<List<byte[]>> replaySpot(
        Staged staged,
        ZLinkSpotAcceptedJournal.Record record) {
        requireActive(staged);
        return backend.replaySpot(staged.spot, record);
    }

    CompletionStage<Optional<byte[]>> replayActor(
        Staged staged,
        ZLinkActorAcceptedJournal.Record record) {
        requireActive(staged);
        for (int index = 0; index < staged.request.actors().size(); index++) {
            if (staged.request.actors().get(index).actorId()
                .equals(record.actorId())) {
                return backend.replayActor(
                    staged.spot, staged.actors.get(index), record);
            }
        }
        return CompletableFuture.failedFuture(new IllegalArgumentException(
            "accepted journal references an Actor outside the aggregate"));
    }

    private static void requireStagingPrefix(
        Request initial,
        Request finalRequest) {
        Objects.requireNonNull(finalRequest, "finalRequest");
        if (initial.spotType() != finalRequest.spotType()
            || !initial.spotStableType().equals(
                finalRequest.spotStableType())
            || !initial.spotId().equals(finalRequest.spotId())
            || initial.objectGeneration() != finalRequest.objectGeneration()
            || initial.restoreSpotSnapshot()
                != finalRequest.restoreSpotSnapshot()
            || !Arrays.equals(
                initial.spotState(), finalRequest.spotState())
            || !Arrays.equals(
                initial.timerEnvelope(), finalRequest.timerEnvelope())
            || !sameActors(initial.actors(), finalRequest.actors())
            || !journalIsPrefix(
                initial.acceptedJournal(),
                finalRequest.acceptedJournal())) {
            throw new IllegalArgumentException(
                "final relocation root does not extend its staging root");
        }
    }

    private static boolean sameActors(
        List<ActorParticipant> initial,
        List<ActorParticipant> finalActors) {
        if (initial.size() != finalActors.size()) {
            return false;
        }
        for (int index = 0; index < initial.size(); index++) {
            ActorParticipant left = initial.get(index);
            ActorParticipant right = finalActors.get(index);
            if (!left.actorId().equals(right.actorId())
                || !left.actorType().equals(right.actorType())
                || left.restoreSnapshot() != right.restoreSnapshot()
                || !Objects.equals(
                    left.preparedActorRef(), right.preparedActorRef())
                || !Arrays.equals(left.state(), right.state())
                || !Arrays.equals(
                    left.timerEnvelope(), right.timerEnvelope())) {
                return false;
            }
        }
        return true;
    }

    private static boolean journalIsPrefix(
        Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> initial,
        Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> finalJournal) {
        for (var lane : initial.entrySet()) {
            List<ZLinkAsyncSerialQueue.QueuedRecord> completed =
                finalJournal.get(lane.getKey());
            if (completed == null
                || completed.size() < lane.getValue().size()) {
                return false;
            }
            for (int index = 0; index < lane.getValue().size(); index++) {
                if (!sameRecord(
                    lane.getValue().get(index),
                    completed.get(index))) {
                    return false;
                }
            }
        }
        return true;
    }

    private static boolean sameRecord(
        ZLinkAsyncSerialQueue.QueuedRecord left,
        ZLinkAsyncSerialQueue.QueuedRecord right) {
        return left.sequence() == right.sequence()
            && Arrays.equals(left.payload(), right.payload());
    }

    CompletionStage<Void> discard(Staged staged) {
        Objects.requireNonNull(staged, "staged");
        List<PendingIngress> pending;
        synchronized (staged) {
            requireActive(staged);
            if (staged.published) {
                return CompletableFuture.failedFuture(new IllegalStateException(
                    "committed aggregate staging cannot roll back to source"));
            }
            staged.ingressClosed = true;
            staged.terminal = true;
            pending = new ArrayList<>(staged.relayedIngress.size()
                + staged.pendingIngress.size());
            pending.addAll(staged.relayedIngress);
            pending.addAll(staged.pendingIngress);
            if (staged.durableBacklog != null) {
                pending.addAll(staged.durableBacklog.relayed);
                pending.addAll(staged.durableBacklog.temporary);
                staged.durableBacklog = null;
            }
            staged.relayedIngress.clear();
            staged.pendingIngress.clear();
        }
        IllegalStateException aborted = new IllegalStateException(
            "aggregate relocation target staging was aborted");
        pending.forEach(ingress -> notifyIngressFailure(ingress, aborted));
        return discardPartial(staged.spot, staged.actors);
    }

    private void requireActive(Staged staged) {
        if (staged == null || staged.owner != this || staged.terminal) {
            throw new IllegalStateException(
                "aggregate staging fence is not active");
        }
    }

    private static void validateJournal(
        Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> journal) {
        for (Map.Entry<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> lane
            : journal.entrySet()) {
            if (lane.getKey() == null || lane.getKey().isBlank()) {
                throw new IllegalArgumentException(
                    "accepted journal lane id is required");
            }
            long previous = 0;
            for (ZLinkAsyncSerialQueue.QueuedRecord record : lane.getValue()) {
                if (record.sequence() <= previous) {
                    throw new IllegalArgumentException(
                        "accepted journal sequence must be strictly increasing");
                }
                previous = record.sequence();
            }
        }
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while (current instanceof CompletionException
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    @FunctionalInterface
    interface JournalReplayer {
        CompletionStage<Void> replay(
            String laneId,
            ZLinkAsyncSerialQueue.QueuedRecord record);

        default CompletionStage<Void> replayFrozen(
            String laneId,
            byte[] frozenRecord) {
            return replay(
                laneId,
                new ZLinkAsyncSerialQueue.QueuedRecord(1, frozenRecord));
        }
    }

    interface StagingBackend {
        CompletionStage<Object> prepareSpot(Request request);

        CompletionStage<Void> restoreSpot(
            Object preparedSpot,
            Request request,
            ZLinkRelocationCancellation cancellation);

        CompletionStage<Object> prepareActor(
            ActorParticipant participant,
            ZLinkRelocationCancellation cancellation);

        default CompletionStage<Void> completeRelocationReady(
            Object preparedSpot) {
            return CompletableFuture.completedFuture(null);
        }

        default Object beginIngressHold(Object preparedSpot) {
            return null;
        }

        default void resumeIngress(Object preparedSpot, Object ingressHold) {
        }

        default <T> CompletionStage<T> admitApplicationJob(
            Supplier<CompletionStage<T>> turn) {
            return Objects.requireNonNull(turn, "turn").get();
        }

        default CompletionStage<Void> stageActorTimers(
            Object preparedSpot,
            Object preparedActor,
            ActorParticipant participant,
            byte[] timerEnvelope) {
            return stageActorTimers(preparedActor, timerEnvelope);
        }

        default CompletionStage<Void> stageActorTimers(
            Object preparedActor,
            byte[] timerEnvelope) {
            if (ZLinkSpotTimerRelocationEnvelope
                    .canonicalize(timerEnvelope).isEmpty()) {
                return CompletableFuture.completedFuture(null);
            }
            return CompletableFuture.failedFuture(new IllegalStateException(
                "staged Actor timer runtime is unavailable"));
        }

        void publishSpot(Object preparedSpot);

        void publishActor(Object preparedActor);

        /**
         * Publishes a relocated Actor and installs its target Spot route and
         * authority fence when the target owner generation is available.
         * Test and non-relocation backends may retain the legacy publication
         * behavior.
         */
        default void publishActor(
            Object preparedActor,
            String targetSpotId,
            long targetOwnerGeneration) {
            publishActor(preparedActor);
        }

        void completeActor(Object preparedActor);

        default void publishActorTimers(
            Object preparedSpot,
            Object preparedActor,
            ActorParticipant participant) {
            if (!ZLinkSpotTimerRelocationEnvelope
                    .canonicalize(participant.timerEnvelope()).isEmpty()) {
                throw new IllegalStateException(
                    "staged Actor timer runtime is unavailable");
            }
        }

        void publishTimers(Object preparedSpot);

        default CompletionStage<List<byte[]>> replaySpot(
            Object preparedSpot,
            ZLinkSpotAcceptedJournal.Record record) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "staged Spot replay is unavailable"));
        }

        default CompletionStage<Optional<byte[]>> replayActor(
            Object preparedSpot,
            Object preparedActor,
            ZLinkActorAcceptedJournal.Record record) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "staged Actor replay is unavailable"));
        }

        CompletionStage<Void> discardActor(Object preparedActor);

        void discardSpot(Object preparedSpot);
    }

    record ActorParticipant(
        String actorId,
        String actorType,
        byte[] state,
        boolean restoreSnapshot,
        ZLinkBackendActorRef preparedActorRef,
        byte[] timerEnvelope) {
        ActorParticipant(
            String actorId,
            String actorType,
            byte[] state,
            boolean restoreSnapshot,
            ZLinkBackendActorRef preparedActorRef) {
            this(
                actorId,
                actorType,
                state,
                restoreSnapshot,
                preparedActorRef,
                ZLinkSpotTimerRelocationEnvelope.encodeCanonical(List.of()));
        }

        ActorParticipant {
            if (actorId == null || actorId.isBlank()
                || actorType == null || actorType.isBlank()) {
                throw new IllegalArgumentException(
                    "Actor id and stable type are required");
            }
            state = Objects.requireNonNull(state, "state").clone();
            timerEnvelope = Objects.requireNonNull(
                timerEnvelope, "timerEnvelope").clone();
            ZLinkSpotTimerRelocationEnvelope.canonicalize(timerEnvelope);
        }

        @Override public byte[] state() { return state.clone(); }
        @Override public byte[] timerEnvelope() {
            return timerEnvelope.clone();
        }
    }

    record Request(
        Class<? extends ZLinkSpot<?>> spotType,
        String spotStableType,
        String spotId,
        long objectGeneration,
        byte[] spotState,
        boolean restoreSpotSnapshot,
        byte[] timerEnvelope,
        List<ActorParticipant> actors,
        Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> acceptedJournal) {
        Request {
            Objects.requireNonNull(spotType, "spotType");
            if (spotStableType == null || spotStableType.isBlank()
                || spotId == null || spotId.isBlank()
                || objectGeneration <= 0) {
                throw new IllegalArgumentException(
                    "Spot stable type, id and generation are required");
            }
            spotState = Objects.requireNonNull(spotState, "spotState").clone();
            timerEnvelope = Objects.requireNonNull(
                timerEnvelope,
                "timerEnvelope").clone();
            actors = List.copyOf(Objects.requireNonNull(actors, "actors"));
            LinkedHashMap<String, List<
                ZLinkAsyncSerialQueue.QueuedRecord>> journalCopy =
                    new LinkedHashMap<>();
            Objects.requireNonNull(acceptedJournal, "acceptedJournal")
                .forEach((lane, records) -> journalCopy.put(
                    lane,
                    List.copyOf(records)));
            acceptedJournal = Collections.unmodifiableMap(journalCopy);
            validateJournal(acceptedJournal);
        }

        @Override public byte[] spotState() { return spotState.clone(); }
        @Override public byte[] timerEnvelope() { return timerEnvelope.clone(); }
    }

    static final class Staged {
        private final ZLinkUserSpotAggregateStagingOwner owner;
        private final Request request;
        private final Object spot;
        private final List<Object> actors;
        private final Object ingressHold;
        private final List<PendingIngress> relayedIngress = new ArrayList<>();
        private final List<PendingIngress> pendingIngress = new ArrayList<>();
        private DurableBacklog durableBacklog;
        private boolean published;
        private boolean ingressClosed;
        private boolean backlogSealed;
        private boolean lifecycleOpen;
        private boolean terminal;

        private Staged(
            ZLinkUserSpotAggregateStagingOwner owner,
            Request request,
            Object spot,
            List<Object> actors,
            Object ingressHold) {
            this.owner = owner;
            this.request = request;
            this.spot = spot;
            this.actors = List.copyOf(actors);
            this.ingressHold = ingressHold;
        }

        int actorCount() {
            return request.actors().size();
        }
    }

    static final class DurableBacklog {
        private final ZLinkUserSpotAggregateStagingOwner owner;
        private final Staged staged;
        private final Request finalRequest;
        private final JournalReplayer replayer;
        private final List<PendingIngress> relayed;
        private final List<PendingIngress> temporary;
        private boolean consumed;

        private DurableBacklog(
            ZLinkUserSpotAggregateStagingOwner owner,
            Staged staged,
            Request finalRequest,
            JournalReplayer replayer,
            List<PendingIngress> relayed,
            List<PendingIngress> temporary) {
            this.owner = owner;
            this.staged = staged;
            this.finalRequest = finalRequest;
            this.replayer = replayer;
            this.relayed = List.copyOf(relayed);
            this.temporary = List.copyOf(temporary);
        }
    }

    private static final class ProductionBackend implements StagingBackend {
        private final ZLinkSpotLifecycle spots;
        private final ZLinkActorRuntime actors;
        private final ZLinkRelocationAdapterRegistry adapters;
        private final ZLinkSpotRuntime runtime;

        private ProductionBackend(
            ZLinkSpotLifecycle spots,
            ZLinkActorRuntime actors,
            ZLinkRelocationAdapterRegistry adapters) {
            this(spots, actors, adapters, null);
        }

        private ProductionBackend(
            ZLinkSpotLifecycle spots,
            ZLinkActorRuntime actors,
            ZLinkRelocationAdapterRegistry adapters,
            ZLinkSpotRuntime runtime) {
            this.spots = spots;
            this.actors = actors;
            this.adapters = adapters;
            this.runtime = runtime;
        }

        @Override
        public CompletionStage<Object> prepareSpot(Request request) {
            return spots.prepareReserved(
                    request.spotType(),
                    request.spotId(),
                    request.objectGeneration(),
                    ZLinkMessage.empty())
                .thenApply(value -> value);
        }

        @Override
        public CompletionStage<Void> restoreSpot(
            Object value,
            Request request,
            ZLinkRelocationCancellation cancellation) {
            var prepared = (ZLinkSpotLifecycle.PreparedUserSpot) value;
            Object spot = spots.preparedSpot(prepared);
            CompletionStage<Void> restore = request.restoreSpotSnapshot()
                ? adapters.restoreSpot(
                    request.spotStableType(),
                    spot,
                    request.spotState(),
                    cancellation)
                : CompletableFuture.completedFuture(null);
            return restore.thenRun(() -> {
                validateJournal(request.acceptedJournal());
                spots.stageReservedTimers(prepared, request.timerEnvelope());
            });
        }

        @Override
        public CompletionStage<Void> completeRelocationReady(Object value) {
            return spots.completeRelocationReady(
                (ZLinkSpotLifecycle.PreparedUserSpot) value);
        }

        @Override
        public Object beginIngressHold(Object value) {
            return spots.beginReservedIngressHold(
                (ZLinkSpotLifecycle.PreparedUserSpot) value);
        }

        @Override
        public void resumeIngress(Object value, Object ingressHold) {
            spots.resumeReservedIngress(
                (ZLinkSpotLifecycle.PreparedUserSpot) value,
                ingressHold);
        }

        @Override
        public <T> CompletionStage<T> admitApplicationJob(
            Supplier<CompletionStage<T>> turn) {
            if (runtime == null) {
                return StagingBackend.super.admitApplicationJob(turn);
            }
            return runtime.admitNewApplicationJob(turn);
        }

        @Override
        public CompletionStage<Object> prepareActor(
            ActorParticipant participant,
            ZLinkRelocationCancellation cancellation) {
            return actors.prepareRelocatedActor(
                    participant.actorId(),
                    participant.actorType(),
                    participant.state(),
                    participant.restoreSnapshot(),
                    adapters,
                    cancellation,
                    participant.preparedActorRef())
                .thenApply(value -> value);
        }

        @Override
        public CompletionStage<Void> stageActorTimers(
            Object preparedSpot,
            Object preparedActor,
            ActorParticipant participant,
            byte[] timerEnvelope) {
            spots.stageReservedActorTimers(
                (ZLinkSpotLifecycle.PreparedUserSpot) preparedSpot,
                participant.actorId(),
                timerEnvelope);
            return CompletableFuture.completedFuture(null);
        }

        @Override public void publishSpot(Object value) {
            spots.publishReserved((ZLinkSpotLifecycle.PreparedUserSpot) value);
        }

        @Override public void publishActor(Object value) {
            actors.publishPreparedTransferredActor(
                (ZLinkActorRuntime.PreparedTransferredActor) value);
        }

        @Override
        public void publishActor(
            Object value,
            String targetSpotId,
            long targetOwnerGeneration) {
            actors.publishPreparedTransferredActor(
                (ZLinkActorRuntime.PreparedTransferredActor) value,
                targetSpotId,
                targetOwnerGeneration);
        }

        @Override public void completeActor(Object value) {
            actors.completePreparedTransferredActor(
                (ZLinkActorRuntime.PreparedTransferredActor) value);
        }

        @Override
        public void publishActorTimers(
            Object preparedSpot,
            Object preparedActor,
            ActorParticipant participant) {
            spots.publishReservedActorTimers(
                (ZLinkSpotLifecycle.PreparedUserSpot) preparedSpot,
                participant.actorId());
        }

        @Override public void publishTimers(Object value) {
            spots.publishReservedTimers(
                (ZLinkSpotLifecycle.PreparedUserSpot) value);
        }

        @Override
        public CompletionStage<List<byte[]>> replaySpot(
            Object value,
            ZLinkSpotAcceptedJournal.Record record) {
            return spots.replayReserved(
                (ZLinkSpotLifecycle.PreparedUserSpot) value,
                record);
        }

        @Override
        public CompletionStage<Optional<byte[]>> replayActor(
            Object preparedSpot,
            Object preparedActor,
            ZLinkActorAcceptedJournal.Record record) {
            if (runtime == null) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "staged Actor replay runtime is unavailable"));
            }
            return runtime.replayPreparedActor(
                (ZLinkSpotLifecycle.PreparedUserSpot) preparedSpot,
                (ZLinkActorRuntime.PreparedTransferredActor) preparedActor,
                record);
        }

        @Override public CompletionStage<Void> discardActor(Object value) {
            return actors.discardPreparedTransferredActor(
                (ZLinkActorRuntime.PreparedTransferredActor) value);
        }

        @Override public void discardSpot(Object value) {
            spots.discardReserved((ZLinkSpotLifecycle.PreparedUserSpot) value);
        }
    }

    private record PendingIngress(
        String laneId,
        byte[] record,
        Consumer<List<Message>> reply,
        Consumer<Throwable> failure) {
        private PendingIngress {
            Objects.requireNonNull(laneId, "laneId");
            record = Objects.requireNonNull(record, "record").clone();
        }

        @Override public byte[] record() {
            return record.clone();
        }
    }
}

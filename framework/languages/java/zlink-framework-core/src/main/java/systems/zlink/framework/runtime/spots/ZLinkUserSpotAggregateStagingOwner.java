package systems.zlink.framework.runtime.spots;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.actors.ZLinkActor;
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
        return backend.prepareSpot(request)
            .thenCompose(preparedSpot -> backend.restoreSpot(
                    preparedSpot,
                    request,
                    cancellation)
                .thenCompose(ignored -> prepareActors(
                    preparedSpot,
                    request,
                    cancellation,
                    preparedActors))
                .thenApply(ignored -> new Staged(
                        this,
                        request,
                        preparedSpot,
                        preparedActors))
                .exceptionallyCompose(failure -> discardPartial(
                    preparedSpot,
                    preparedActors)
                    .thenCompose(ignored -> CompletableFuture.failedFuture(
                        unwrap(failure)))));
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

    CompletionStage<Void> publishAndReplayHidden(
        Staged staged,
        JournalReplayer replayer) {
        return publishAndReplayHidden(staged, staged.request, replayer);
    }

    CompletionStage<Void> publishAndReplay(
        Staged staged,
        Request finalRequest,
        JournalReplayer replayer) {
        return publishAndReplayHidden(staged, finalRequest, replayer)
            .thenRun(() -> openAdmission(staged));
    }

    CompletionStage<Void> publishAndReplayHidden(
        Staged staged,
        Request finalRequest,
        JournalReplayer replayer) {
        requireActive(staged);
        requireStagingPrefix(staged.request, finalRequest);
        Objects.requireNonNull(replayer, "replayer");
        // Replay the final authority-selected journal while the prepared
        // objects remain hidden. Local Ready/admission opens only after every
        // captured and held suffix record completes.
        CompletionStage<Void> replay =
            backend.completeRelocationReady(staged.spot);
        for (Map.Entry<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> lane
            : finalRequest.acceptedJournal().entrySet()) {
            for (ZLinkAsyncSerialQueue.QueuedRecord record : lane.getValue()) {
                replay = replay.thenCompose(ignored -> replayer.replay(
                    lane.getKey(),
                    record));
            }
        }
        return replay.thenRun(() -> {
            for (var actor : staged.actors) {
                backend.publishActor(actor);
            }
            backend.publishSpot(staged.spot);
            staged.published = true;
        });
    }

    void openAdmission(Staged staged) {
        requireActive(staged);
        if (!staged.published) {
            throw new IllegalStateException(
                "aggregate staging is not published");
        }
        for (int index = 0; index < staged.actors.size(); index++) {
            Object actor = staged.actors.get(index);
            ActorParticipant participant = staged.request.actors().get(index);
            backend.completeActor(actor);
            backend.publishActorTimers(
                staged.spot, actor, participant);
        }
        backend.publishTimers(staged.spot);
        staged.terminal = true;
    }

    CompletionStage<List<byte[]>> replaySpot(
        Staged staged,
        ZLinkSpotAcceptedJournal.Record record) {
        requireActive(staged);
        return backend.replaySpot(staged.spot, record);
    }

    CompletionStage<java.util.Optional<byte[]>> replayActor(
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
            || !java.util.Arrays.equals(
                initial.spotState(), finalRequest.spotState())
            || !java.util.Arrays.equals(
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
                || !java.util.Arrays.equals(left.state(), right.state())
                || !java.util.Arrays.equals(
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
            && java.util.Arrays.equals(left.payload(), right.payload());
    }

    CompletionStage<Void> discard(Staged staged) {
        requireActive(staged);
        if (staged.published) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "committed aggregate staging cannot roll back to source"));
        }
        staged.terminal = true;
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

        default CompletionStage<java.util.Optional<byte[]>> replayActor(
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
            java.util.LinkedHashMap<String, List<
                ZLinkAsyncSerialQueue.QueuedRecord>> journalCopy =
                    new java.util.LinkedHashMap<>();
            Objects.requireNonNull(acceptedJournal, "acceptedJournal")
                .forEach((lane, records) -> journalCopy.put(
                    lane,
                    List.copyOf(records)));
            acceptedJournal = java.util.Collections.unmodifiableMap(journalCopy);
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
        private boolean published;
        private boolean terminal;

        private Staged(
            ZLinkUserSpotAggregateStagingOwner owner,
            Request request,
            Object spot,
            List<Object> actors) {
            this.owner = owner;
            this.request = request;
            this.spot = spot;
            this.actors = List.copyOf(actors);
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
        public CompletionStage<java.util.Optional<byte[]>> replayActor(
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
}

package systems.zlink.framework.runtime.spots;

import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletionStage;
import java.util.function.BooleanSupplier;
import java.util.function.Predicate;
import java.util.function.Supplier;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkCompositeRelocationBarrier;

/**
 * Owns the source-side all-lane barrier for one User Spot aggregate.
 */
final class ZLinkUserSpotRelocationBarrier {
    private final DefaultSpotContext context;
    private final ZLinkActorSessionCoordinator actors;
    private final ZLinkCompositeRelocationBarrier barrier =
        new ZLinkCompositeRelocationBarrier();
    private Seal active;

    ZLinkUserSpotRelocationBarrier(
        DefaultSpotContext context,
        ZLinkActorSessionCoordinator actors) {
        this.context = java.util.Objects.requireNonNull(
            context, "context");
        this.actors = java.util.Objects.requireNonNull(
            actors, "actors");
    }

    synchronized Optional<Seal> trySeal() {
        return trySeal(ignored -> true);
    }

    synchronized Optional<Seal> trySeal(Predicate<Preview> admission) {
        java.util.Objects.requireNonNull(admission, "admission");
        if (active != null) {
            return Optional.empty();
        }
        byte[] timerEnvelope =
            context.freezeTimerRelocationEnvelope();
        List<String> participantActorIds =
            actors.actorIdsInSpot(context.spotId());
        LinkedHashMap<String, ZLinkAsyncSerialQueue> lanes =
            relocationLanes(participantActorIds);
        Optional<ZLinkCompositeRelocationBarrier.Seal> localSeal =
            barrier.trySeal(lanes);
        if (localSeal.isEmpty()) {
            context.resumeTimersAfterRelocationAbort();
            return Optional.empty();
        }
        List<String> currentActorIds =
            actors.actorIdsInSpot(context.spotId());
        if (!participantActorIds.equals(currentActorIds)) {
            rollback(localSeal.get());
            context.resumeTimersAfterRelocationAbort();
            return Optional.empty();
        }
        Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> captured =
            captureRecords(localSeal.get());
        boolean admitted;
        try {
            admitted = admission.test(new Preview(
                timerEnvelope,
                participantActorIds,
                captured));
        } catch (RuntimeException failure) {
            rollback(localSeal.get());
            context.resumeTimersAfterRelocationAbort();
            throw failure;
        }
        if (!admitted) {
            rollback(localSeal.get());
            context.resumeTimersAfterRelocationAbort();
            return Optional.empty();
        }
        active = new Seal(
            localSeal.get(),
            timerEnvelope.clone(),
            participantActorIds,
            captured);
        return Optional.of(active);
    }

    CompletionStage<Optional<Seal>> sealAtTurnBoundary(
        Predicate<Preview> admission,
        BooleanSupplier cancelled) {
        java.util.Objects.requireNonNull(admission, "admission");
        java.util.Objects.requireNonNull(cancelled, "cancelled");
        List<String> participantActorIds;
        LinkedHashMap<String, ZLinkAsyncSerialQueue> lanes;
        synchronized (this) {
            if (active != null) {
                return java.util.concurrent.CompletableFuture
                    .completedFuture(Optional.empty());
            }
            participantActorIds =
                actors.actorIdsInSpot(context.spotId());
            lanes = relocationLanes(participantActorIds);
        }
        return barrier.sealAtTurnBoundary(lanes, cancelled)
            .thenApply(sealed -> finishTurnBoundarySeal(
                sealed,
                participantActorIds,
                admission,
                cancelled));
    }

    CompletionStage<Optional<Seal>> sealForRelocation(
        Predicate<Preview> admission,
        BooleanSupplier cancelled) {
        if (context.relocationReadiness()
            != systems.zlink.framework.configuration
                .ZLinkSpotRelocationReadinessMode.APPLICATION_SIGNALED) {
            return sealAtTurnBoundary(admission, cancelled);
        }
        return context.awaitRelocationReadySignal(
            () -> trySeal(admission),
            cancelled)
            .thenApply(sealed -> {
                sealed.ifPresent(Seal::markApplicationSignaled);
                return sealed;
            });
    }

    private synchronized Optional<Seal> finishTurnBoundarySeal(
        Optional<ZLinkCompositeRelocationBarrier.Seal> sealed,
        List<String> participantActorIds,
        Predicate<Preview> admission,
        BooleanSupplier cancelled) {
        if (sealed.isEmpty()) {
            return Optional.empty();
        }
        ZLinkCompositeRelocationBarrier.Seal composite =
            sealed.orElseThrow();
        if (active != null
            || cancelled.getAsBoolean()
            || !participantActorIds.equals(
                actors.actorIdsInSpot(context.spotId()))) {
            rollback(composite);
            return Optional.empty();
        }
        byte[] timerEnvelope =
            context.freezeTimerRelocationEnvelope();
        Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> captured =
            captureRecords(composite);
        boolean admitted;
        try {
            admitted = admission.test(new Preview(
                timerEnvelope,
                participantActorIds,
                captured));
        } catch (RuntimeException failure) {
            rollback(composite);
            context.resumeTimersAfterRelocationAbort();
            throw failure;
        }
        if (!admitted) {
            rollback(composite);
            context.resumeTimersAfterRelocationAbort();
            return Optional.empty();
        }
        active = new Seal(
            composite,
            timerEnvelope,
            participantActorIds,
            captured);
        return Optional.of(active);
    }

    synchronized <T> CompletionStage<T> runCapture(
        Seal seal,
        Supplier<CompletionStage<T>> capture) {
        requireActive(seal);
        return barrier.runCapture(seal.composite, capture);
    }

    synchronized boolean abort(Seal seal) {
        if (seal == null || seal != active) {
            return false;
        }
        RuntimeException completionFailure = null;
        if (seal.applicationSignaled()
            && seal.markCompletionScheduled()) {
            try {
                context.runRelocationReadyCompletion(
                    systems.zlink.framework.spots
                        .ZLinkSpotRelocationReadyOutcome.CONTINUED)
                    .toCompletableFuture().join();
            } catch (RuntimeException failure) {
                completionFailure = failure;
            }
        }
        if (!barrier.abort(seal.composite)) {
            throw new IllegalStateException(
                "User Spot barrier abort lost local lane");
        }
        active = null;
        context.resumeTimersAfterRelocationAbort();
        if (completionFailure != null) {
            throw completionFailure;
        }
        return true;
    }

    synchronized Optional<Committed> commit(Seal seal) {
        if (seal == null || seal != active) {
            return Optional.empty();
        }
        LinkedHashMap<String, List<ZLinkAsyncSerialQueue.QueuedRecord>>
            heldIngress = new LinkedHashMap<>(
            barrier.commit(seal.composite)
                .orElseThrow(() -> new IllegalStateException(
                    "User Spot barrier commit lost a lane")));
        active = null;
        return Optional.of(new Committed(
            seal.generation(),
            seal.timerEnvelope(),
            seal.participantActorIds(),
            heldIngress));
    }

    synchronized Optional<Map<String, List<
        ZLinkAsyncSerialQueue.QueuedRecord>>> freezeIngress(Seal seal) {
        if (seal == null || seal != active) {
            return Optional.empty();
        }
        LinkedHashMap<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> held =
            new LinkedHashMap<>();
        held.putAll(barrier.freezeIngress(seal.composite)
            .orElseThrow(() -> new IllegalStateException(
                "User Spot barrier freeze lost a lane")));
        return Optional.of(java.util.Collections.unmodifiableMap(held));
    }

    private void rollback(ZLinkCompositeRelocationBarrier.Seal seal) {
        if (!barrier.abort(seal)) {
            throw new IllegalStateException(
                "partial User Spot barrier rollback lost a lane");
        }
    }

    private void requireActive(Seal seal) {
        if (seal == null || seal != active) {
            throw new IllegalStateException(
                "capture requires the active User Spot barrier generation");
        }
    }

    private Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>>
        captureRecords(ZLinkCompositeRelocationBarrier.Seal seal) {
        return new LinkedHashMap<>(barrier.captured(seal)
                .orElseThrow(() -> new IllegalStateException(
                    "User Spot relocation seal is not active")));
    }

    private LinkedHashMap<String, ZLinkAsyncSerialQueue> relocationLanes(
        List<String> participantActorIds) {
        LinkedHashMap<String, ZLinkAsyncSerialQueue> lanes =
            new LinkedHashMap<>(context.relocationLanes());
        for (String actorId : participantActorIds) {
            if (lanes.putIfAbsent(
                    "actor:" + actorId,
                    actors.actorRelocationLane(actorId)) != null) {
                throw new IllegalStateException(
                    "duplicate User Spot relocation lane: actor:"
                        + actorId);
            }
        }
        return lanes;
    }

    static final class Seal {
        private final ZLinkCompositeRelocationBarrier.Seal composite;
        private final byte[] timerEnvelope;
        private final List<String> participantActorIds;
        private final Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>>
            capturedRecords;
        private boolean applicationSignaled;
        private boolean completionScheduled;

        private Seal(
            ZLinkCompositeRelocationBarrier.Seal composite,
            byte[] timerEnvelope,
            List<String> participantActorIds,
            Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>>
                capturedRecords) {
            this.composite = composite;
            this.timerEnvelope = timerEnvelope;
            this.participantActorIds = List.copyOf(
                participantActorIds);
            this.capturedRecords = capturedRecords;
        }

        long generation() {
            return composite.generation();
        }

        byte[] timerEnvelope() {
            return timerEnvelope.clone();
        }

        List<String> participantActorIds() {
            return participantActorIds;
        }

        Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>>
            capturedRecords() {
            return capturedRecords;
        }

        synchronized void markApplicationSignaled() {
            applicationSignaled = true;
        }

        synchronized boolean applicationSignaled() {
            return applicationSignaled;
        }

        synchronized boolean markCompletionScheduled() {
            if (completionScheduled) {
                return false;
            }
            completionScheduled = true;
            return true;
        }
    }

    record Preview(
        byte[] timerEnvelope,
        List<String> participantActorIds,
        Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> capturedRecords) {
        Preview {
            timerEnvelope = timerEnvelope.clone();
            participantActorIds = List.copyOf(participantActorIds);
            capturedRecords = Map.copyOf(capturedRecords);
        }

        @Override public byte[] timerEnvelope() {
            return timerEnvelope.clone();
        }
    }

    record Committed(
        long generation,
        byte[] timerEnvelope,
        List<String> participantActorIds,
        Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> heldIngress) {
        Committed {
            timerEnvelope = timerEnvelope.clone();
            participantActorIds = List.copyOf(participantActorIds);
            heldIngress = Map.copyOf(heldIngress);
        }

        @Override
        public byte[] timerEnvelope() {
            return timerEnvelope.clone();
        }
    }
}

package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.locations.ZLinkServiceRelocationEnvelopeCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkActorJoinRecoveryCodec;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Objects;
import java.util.UUID;

/** Projects one independent Actor onto relocation-envelope-v1. */
public final class ZLinkCanonicalActorRelocationEnvelope {
    static final long PARTICIPANT_ID = 1;

    private ZLinkCanonicalActorRelocationEnvelope() {}

    public static byte[] encode(
            UUID relocationId,
            String actorId,
            long objectGeneration,
            long expectedAuthorityOwnerGeneration,
            boolean restoreSnapshot,
            byte[] state,
            List<ZLinkSerialExecutionQueue.QueuedRecord> journal) {
        return encode(
                relocationId,
                actorId,
                objectGeneration,
                expectedAuthorityOwnerGeneration,
                restoreSnapshot,
                state,
                journal,
                ZLinkSpotTimerRelocationEnvelope.encodeCanonical(List.of()),
                null);
    }

    static byte[] encode(
            UUID relocationId,
            String actorId,
            long objectGeneration,
            long expectedAuthorityOwnerGeneration,
            boolean restoreSnapshot,
            byte[] state,
            List<ZLinkSerialExecutionQueue.QueuedRecord> journal,
            byte[] timerEnvelope) {
        return encode(
                relocationId,
                actorId,
                objectGeneration,
                expectedAuthorityOwnerGeneration,
                restoreSnapshot,
                state,
                journal,
                timerEnvelope,
                null);
    }

    static byte[] encode(
            UUID relocationId,
            String actorId,
            long objectGeneration,
            long expectedAuthorityOwnerGeneration,
            boolean restoreSnapshot,
            byte[] state,
            List<ZLinkSerialExecutionQueue.QueuedRecord> journal,
            byte[] timerEnvelope,
            byte[] actorJoinRecovery) {
        return encode(
                relocationId,
                actorId,
                objectGeneration,
                expectedAuthorityOwnerGeneration,
                1,
                restoreSnapshot,
                state,
                journal,
                timerEnvelope,
                actorJoinRecovery);
    }

    static byte[] encode(
            UUID relocationId,
            String actorId,
            long objectGeneration,
            long expectedAuthorityOwnerGeneration,
            long applicationVersion,
            boolean restoreSnapshot,
            byte[] state,
            List<ZLinkSerialExecutionQueue.QueuedRecord> journal,
            byte[] timerEnvelope,
            byte[] actorJoinRecovery) {
        return encode(
                relocationId,
                actorId,
                objectGeneration,
                expectedAuthorityOwnerGeneration,
                applicationVersion,
                restoreSnapshot,
                state,
                journal,
                timerEnvelope,
                actorJoinRecovery,
                List.of());
    }

    static byte[] encode(
            UUID relocationId,
            String actorId,
            long objectGeneration,
            long expectedAuthorityOwnerGeneration,
            long applicationVersion,
            boolean restoreSnapshot,
            byte[] state,
            List<ZLinkSerialExecutionQueue.QueuedRecord> journal,
            byte[] timerEnvelope,
            byte[] actorJoinRecovery,
            List<ServiceWirePilotCodec.RelocationSessionBinding> bindings) {
        Objects.requireNonNull(relocationId, "relocationId");
        Objects.requireNonNull(actorId, "actorId");
        byte[] applicationState = Objects.requireNonNull(state, "state").clone();
        List<ZLinkSerialExecutionQueue.QueuedRecord> records =
                new ArrayList<>(Objects.requireNonNull(journal, "journal"));
        records.sort(Comparator.comparingLong(ZLinkSerialExecutionQueue.QueuedRecord::sequence));
        long previous = 0;
        for (var record : records) {
            if (Long.compareUnsigned(record.sequence(), previous) <= 0) {
                throw new IllegalArgumentException("Actor journal sequence is not canonical");
            }
            previous = record.sequence();
        }
        List<ZLinkSpotTimerRelocationEnvelope.CanonicalTimer> timers =
                new ArrayList<>(ZLinkSpotTimerRelocationEnvelope.canonicalize(timerEnvelope));
        timers.sort(Comparator.comparing(ZLinkSpotTimerRelocationEnvelope.CanonicalTimer::name));
        boolean hasActorJoinRecovery = actorJoinRecovery != null;
        byte[] recovery = hasActorJoinRecovery ? actorJoinRecovery.clone() : null;
        if (hasActorJoinRecovery && !ZLinkActorJoinRecoveryCodec.isRecoverySavedWork(recovery)) {
            throw new IllegalArgumentException(
                    "Actor Join recovery saved work is not canonical ZLJR");
        }
        long acceptedBoundary = records.isEmpty() ? 0 : records.getLast().sequence();
        long actorJoinRecoverySequence = 0;
        if (hasActorJoinRecovery) {
            actorJoinRecoverySequence = Math.incrementExact(acceptedBoundary);
            acceptedBoundary = actorJoinRecoverySequence;
        }
        List<PendingTimer> pendingTimers = new ArrayList<>();
        for (var timer : timers) {
            if (timer.pending() != null) {
                acceptedBoundary = Math.incrementExact(acceptedBoundary);
                pendingTimers.add(new PendingTimer(acceptedBoundary, timer));
            }
        }

        List<ServiceWirePilotCodec.RelocationSavedWork> work = new ArrayList<>();
        for (var record : records) {
            if (systems.zlink.framework.runtime.internal.service.ZLinkServiceFrozenRecordCodec
                    .isCanonical(record.payload())) {
                work.add(
                        new ServiceWirePilotCodec.RelocationSavedWork(
                                PARTICIPANT_ID, record.sequence(), record.payload()));
            }
        }
        if (hasActorJoinRecovery) {
            work.add(
                    new ServiceWirePilotCodec.RelocationSavedWork(
                            PARTICIPANT_ID, actorJoinRecoverySequence, recovery));
        }
        var registrations =
                timers.stream()
                        .map(
                                timer ->
                                        new ServiceWirePilotCodec.RelocationTimerRegistration(
                                                PARTICIPANT_ID,
                                                timer.name(),
                                                timer.handlerType(),
                                                timer.periodMilliseconds(),
                                                timer.overrunPolicy(),
                                                timer.maxCatchUpTicks(),
                                                timer.stopOnUnhandledException(),
                                                timer.lastCompletedDeliveryIndex(),
                                                timer.lastCompletedScheduledIndex(),
                                                timer.nextScheduledAtUnixMilliseconds()))
                        .toList();
        var pending =
                pendingTimers.stream()
                        .map(
                                value -> {
                                    var timer = value.timer();
                                    var tick = timer.pending();
                                    return new ServiceWirePilotCodec.RelocationPendingTimerTick(
                                            PARTICIPANT_ID,
                                            value.sequence(),
                                            timer.name(),
                                            tick.deliveryIndex(),
                                            tick.scheduledIndex(),
                                            tick.scheduledAtUnixMilliseconds(),
                                            tick.skippedTicks());
                                })
                        .toList();
        try {
            return ServiceWirePilotCodec.encodeRelocationEnvelopeV1(
                    new ServiceWirePilotCodec.RelocationEnvelopeV1(
                            relocationId.getMostSignificantBits(),
                            relocationId.getLeastSignificantBits(),
                            new ServiceWirePilotCodec.RelocationActorIdentity(
                                    actorId, objectGeneration, expectedAuthorityOwnerGeneration),
                            applicationVersion,
                            List.of(
                                    new ServiceWirePilotCodec.RelocationApplicationState(
                                            PARTICIPANT_ID,
                                            restoreSnapshot,
                                            restoreSnapshot ? applicationState : new byte[0])),
                            work,
                            registrations,
                            pending,
                            bindings));
        } catch (IOException failure) {
            throw new IllegalArgumentException("canonical Actor relocation envelope", failure);
        }
    }

    static Decoded decode(
            byte[] encoded, UUID relocationId, String actorId, boolean restoreSnapshot) {
        var root = ZLinkServiceRelocationEnvelopeCodec.decode(encoded);
        if (root.relocationHigh() != relocationId.getMostSignificantBits()
                || root.relocationLow() != relocationId.getLeastSignificantBits()
                || root.object().kind() != ZLinkPlacementObjectKind.ACTOR.value()
                || !root.object().objectId().equals(actorId)
                || root.applicationStates().size() != 1) {
            throw new IllegalArgumentException(
                    "canonical Actor relocation identity or inventory differs");
        }
        var state = root.applicationStates().getFirst();
        if (state.participantId() != PARTICIPANT_ID || state.hasState() != restoreSnapshot) {
            throw new IllegalArgumentException("canonical Actor relocation state policy differs");
        }
        List<ZLinkSerialExecutionQueue.QueuedRecord> journal =
                root.savedWork().stream()
                        .filter(
                                value ->
                                        !ZLinkActorJoinRecoveryCodec.isRecoverySavedWork(
                                                value.frozenRecord()))
                        .map(
                                value -> {
                                    if (value.participantId() != PARTICIPANT_ID) {
                                        throw new IllegalArgumentException(
                                                "Actor journal references another participant");
                                    }
                                    return new ZLinkSerialExecutionQueue.QueuedRecord(
                                            value.sequence(), value.frozenRecord());
                                })
                        .toList();
        return new Decoded(
                root.object().objectGeneration(),
                root.object().expectedAuthorityOwnerGeneration(),
                state.payload(),
                journal,
                ZLinkSpotTimerRelocationEnvelope.encodeCanonical(
                        root.timerRegistrations().stream()
                                .map(
                                        registration -> {
                                            var pending =
                                                    root.pendingTimerTicks().stream()
                                                            .filter(
                                                                    value ->
                                                                            value.participantId()
                                                                                            == 1
                                                                                    && value.timerName()
                                                                                            .equals(
                                                                                                    registration
                                                                                                            .name()))
                                                            .findFirst()
                                                            .map(
                                                                    value ->
                                                                            new ZLinkSpotTimerRelocationEnvelope
                                                                                    .CanonicalPending(
                                                                                    value
                                                                                            .deliveryIndex(),
                                                                                    value
                                                                                            .scheduledIndex(),
                                                                                    value
                                                                                            .scheduledAtUnixMilliseconds(),
                                                                                    value
                                                                                            .skippedTicks()))
                                                            .orElse(null);
                                            return new ZLinkSpotTimerRelocationEnvelope
                                                    .CanonicalTimer(
                                                    registration.name(),
                                                    registration.handlerType(),
                                                    registration.periodMilliseconds(),
                                                    registration.overrunPolicy(),
                                                    registration.maxCatchUpTicks(),
                                                    registration.stopOnUnhandledException(),
                                                    registration.lastCompletedDeliveryIndex(),
                                                    registration.lastCompletedScheduledIndex(),
                                                    registration.nextScheduledAtUnixMilliseconds(),
                                                    pending);
                                        })
                                .toList()));
    }

    record Decoded(
            long objectGeneration,
            long expectedAuthorityOwnerGeneration,
            byte[] state,
            List<ZLinkSerialExecutionQueue.QueuedRecord> journal,
            byte[] timerEnvelope) {
        Decoded {
            state = state.clone();
            journal = List.copyOf(journal);
            timerEnvelope = timerEnvelope.clone();
        }

        @Override
        public byte[] state() {
            return state.clone();
        }

        @Override
        public byte[] timerEnvelope() {
            return timerEnvelope.clone();
        }
    }

    private record PendingTimer(
            long sequence, ZLinkSpotTimerRelocationEnvelope.CanonicalTimer timer) {}
}

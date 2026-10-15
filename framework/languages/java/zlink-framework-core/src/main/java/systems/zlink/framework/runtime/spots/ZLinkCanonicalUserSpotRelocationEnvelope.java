package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.locations.ZLinkServiceRelocationEnvelopeCodec;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;
import systems.zlink.framework.spots.ZLinkSpot;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.UUID;
import java.util.function.Function;

/** Projects one User Spot aggregate onto canonical relocation-envelope-v1. */
final class ZLinkCanonicalUserSpotRelocationEnvelope {
    private ZLinkCanonicalUserSpotRelocationEnvelope() {}

    static byte[] encode(
            ZLinkUserSpotAggregateStagingOwner.Request request,
            UUID relocationId,
            long expectedAuthorityOwnerGeneration,
            List<ZLinkSpotRetireControl.ParticipantFence> participants) {
        return encode(
                request, relocationId, expectedAuthorityOwnerGeneration, participants, List.of());
    }

    static byte[] encode(
            ZLinkUserSpotAggregateStagingOwner.Request request,
            UUID relocationId,
            long expectedAuthorityOwnerGeneration,
            List<ZLinkSpotRetireControl.ParticipantFence> participants,
            List<ServiceWirePilotCodec.RelocationSessionBinding> bindings) {
        Objects.requireNonNull(request, "request");
        Objects.requireNonNull(relocationId, "relocationId");
        List<ZLinkSpotRetireControl.ParticipantFence> inventory = List.copyOf(participants);
        Map<String, ZLinkUserSpotAggregateStagingOwner.ActorParticipant> actors = new HashMap<>();
        request.actors().forEach(actor -> actors.put(actor.actorId(), actor));
        List<ParticipantTimer> timers = new ArrayList<>();
        List<Journal> journal = new ArrayList<>();
        Map<Long, Long> boundaries = new HashMap<>();
        for (var lane : request.acceptedJournal().entrySet()) {
            long participantId = participantId(inventory, lane.getKey());
            for (var record : lane.getValue()) {
                //  Same rule as the Actor envelope: only canonical
                //  service-wire-v1 frozen records can be replayed, so anything
                //  else must not be encoded or the reader takes the next field
                //  as its record `kind`. The accepted boundary still counts
                //  the turn.
                if (systems.zlink.framework.runtime.internal.service.ZLinkServiceFrozenRecordCodec
                        .isCanonical(record.payload())) {
                    journal.add(new Journal(participantId, record.sequence(), record.payload()));
                }
                boundaries.merge(participantId, record.sequence(), Math::max);
            }
        }
        journal.sort(
                Comparator.comparingLong(Journal::participantId)
                        .thenComparingLong(Journal::sequence));
        long spotParticipant = participantId(inventory, "spot");
        ZLinkSpotTimerRelocationEnvelope.canonicalize(request.timerEnvelope())
                .forEach(timer -> timers.add(new ParticipantTimer(spotParticipant, timer)));
        for (var actor : request.actors()) {
            long actorParticipant = participantId(inventory, "actor:" + actor.actorId());
            ZLinkSpotTimerRelocationEnvelope.canonicalize(actor.timerEnvelope())
                    .forEach(timer -> timers.add(new ParticipantTimer(actorParticipant, timer)));
        }
        timers.sort(
                Comparator.comparingLong(ParticipantTimer::participantId)
                        .thenComparing(value -> value.timer().name()));
        List<PendingTimer> pendingTimers = new ArrayList<>();
        for (ParticipantTimer value : timers) {
            long participant = value.participantId();
            var timer = value.timer();
            if (timer.pending() == null) {
                continue;
            }
            long nextSequence = boundaries.getOrDefault(participant, 0L);
            nextSequence = Math.incrementExact(nextSequence);
            pendingTimers.add(new PendingTimer(participant, nextSequence, timer));
            boundaries.put(participant, nextSequence);
        }

        List<ServiceWirePilotCodec.RelocationApplicationState> states = new ArrayList<>();
        for (int index = 0; index < inventory.size(); index++) {
            var participant = inventory.get(index);
            byte[] state;
            if (participant.objectKind() == ZLinkPlacementObjectKind.USER_SPOT.value()) {
                state = request.spotState();
            } else {
                var actor = actors.get(participant.objectId());
                if (actor == null) throw invalid("Actor inventory differs from captured state");
                state = actor.state();
            }
            states.add(
                    new ServiceWirePilotCodec.RelocationApplicationState(
                            index + 1L,
                            participant.restoreSnapshot(),
                            participant.restoreSnapshot() ? state : new byte[0]));
        }
        var work =
                journal.stream()
                        .map(
                                entry ->
                                        new ServiceWirePilotCodec.RelocationSavedWork(
                                                entry.participantId(),
                                                entry.sequence(),
                                                entry.record()))
                        .toList();
        var registrations =
                timers.stream()
                        .map(
                                value -> {
                                    var timer = value.timer();
                                    return new ServiceWirePilotCodec.RelocationTimerRegistration(
                                            value.participantId(),
                                            timer.name(),
                                            timer.handlerType(),
                                            timer.periodMilliseconds(),
                                            timer.overrunPolicy(),
                                            timer.maxCatchUpTicks(),
                                            timer.stopOnUnhandledException(),
                                            timer.lastCompletedDeliveryIndex(),
                                            timer.lastCompletedScheduledIndex(),
                                            timer.nextScheduledAtUnixMilliseconds());
                                })
                        .toList();
        var pending =
                pendingTimers.stream()
                        .map(
                                value -> {
                                    var timer = value.timer();
                                    var tick = timer.pending();
                                    return new ServiceWirePilotCodec.RelocationPendingTimerTick(
                                            value.participantId(),
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
                            new ServiceWirePilotCodec.RelocationUserSpotIdentity(
                                    request.spotId(),
                                    request.objectGeneration(),
                                    expectedAuthorityOwnerGeneration),
                            1,
                            states,
                            work,
                            registrations,
                            pending,
                            bindings));
        } catch (IOException failure) {
            throw invalid("canonical User Spot relocation envelope: " + failure.getMessage());
        }
    }

    static ZLinkUserSpotAggregateStagingOwner.Request decode(
            byte[] encoded,
            RoutingId targetNodeRid,
            Function<String, Class<? extends ZLinkSpot<?>>> spotTypes,
            ZLinkSpotRetireControl.StageRequest stage) {
        var root = ZLinkServiceRelocationEnvelopeCodec.decode(encoded);
        if (root.relocationHigh() != stage.fence().aggregateId().getMostSignificantBits()
                || root.relocationLow() != stage.fence().aggregateId().getLeastSignificantBits()
                || root.object().kind() != ZLinkPlacementObjectKind.USER_SPOT.value()
                || !root.object().objectId().equals(stage.spotId())) {
            throw invalid("canonical relocation identity differs from stage");
        }
        List<ZLinkSpotRetireControl.ParticipantFence> inventory = stage.participants();
        if (root.applicationStates().size() != inventory.size()) {
            throw invalid("canonical participant inventory is incomplete");
        }
        Map<Long, ZLinkServiceRelocationEnvelopeCodec.ApplicationState> states = new HashMap<>();
        root.applicationStates().forEach(value -> states.put(value.participantId(), value));
        List<ZLinkUserSpotAggregateStagingOwner.ActorParticipant> actors = new ArrayList<>();
        byte[] spotState = null;
        long spotGeneration = 0;
        long spotParticipant = 0;
        Map<Long, String> lanes = new HashMap<>();
        Map<Long, Integer> actorIndexes = new HashMap<>();
        for (int index = 0; index < inventory.size(); index++) {
            long id = index + 1L;
            var participant = inventory.get(index);
            var state = states.get(id);
            if (state == null || state.hasState() != participant.restoreSnapshot()) {
                throw invalid("canonical participant state policy differs");
            }
            if (participant.objectKind() == ZLinkPlacementObjectKind.USER_SPOT.value()) {
                if (!participant.objectId().equals(stage.spotId())) {
                    throw invalid("canonical Spot inventory differs");
                }
                spotState = state.payload();
                spotGeneration = participant.objectGeneration();
                spotParticipant = id;
                lanes.put(id, "spot");
            } else {
                actorIndexes.put(id, actors.size());
                actors.add(
                        new ZLinkUserSpotAggregateStagingOwner.ActorParticipant(
                                participant.objectId(),
                                participant.stableType(),
                                state.payload(),
                                participant.restoreSnapshot(),
                                new ZLinkBackendActorRef(
                                        targetNodeRid,
                                        participant.objectId(),
                                        participant.objectGeneration())));
                lanes.put(id, "actor:" + participant.objectId());
            }
        }
        if (spotState == null) {
            throw invalid("canonical Spot participant is missing");
        }
        LinkedHashMap<String, List<ZLinkSerialExecutionQueue.QueuedRecord>> journal =
                new LinkedHashMap<>();
        for (var entry : root.savedWork()) {
            String lane = lanes.get(entry.participantId());
            if (lane == null) {
                throw invalid("canonical journal participant is unknown");
            }
            journal.computeIfAbsent(lane, ignored -> new ArrayList<>())
                    .add(
                            new ZLinkSerialExecutionQueue.QueuedRecord(
                                    entry.sequence(), entry.frozenRecord()));
        }
        Map<TimerKey, ZLinkServiceRelocationEnvelopeCodec.PendingTimerTick> pendingByName =
                new HashMap<>();
        for (var pending : root.pendingTimerTicks()) {
            if (!lanes.containsKey(pending.participantId())) {
                throw invalid("canonical timer participant is unknown");
            }
            TimerKey key = new TimerKey(pending.participantId(), pending.timerName());
            if (pendingByName.putIfAbsent(key, pending) != null) {
                throw invalid("canonical pending timer is duplicated");
            }
        }
        Map<Long, List<ZLinkSpotTimerRelocationEnvelope.CanonicalTimer>> timersByParticipant =
                new HashMap<>();
        for (var value : root.timerRegistrations()) {
            if (!lanes.containsKey(value.participantId())) {
                throw invalid("canonical timer participant is unknown");
            }
            TimerKey key = new TimerKey(value.participantId(), value.name());
            var pending = pendingByName.remove(key);
            var timer =
                    new ZLinkSpotTimerRelocationEnvelope.CanonicalTimer(
                            value.name(),
                            value.handlerType(),
                            value.periodMilliseconds(),
                            value.overrunPolicy(),
                            value.maxCatchUpTicks(),
                            value.stopOnUnhandledException(),
                            value.lastCompletedDeliveryIndex(),
                            value.lastCompletedScheduledIndex(),
                            value.nextScheduledAtUnixMilliseconds(),
                            pending == null
                                    ? null
                                    : new ZLinkSpotTimerRelocationEnvelope.CanonicalPending(
                                            pending.deliveryIndex(),
                                            pending.scheduledIndex(),
                                            pending.scheduledAtUnixMilliseconds(),
                                            pending.skippedTicks()));
            timersByParticipant
                    .computeIfAbsent(value.participantId(), ignored -> new ArrayList<>())
                    .add(timer);
        }
        if (!pendingByName.isEmpty()) {
            throw invalid("canonical pending timer has no logical registration");
        }
        for (var entry : actorIndexes.entrySet()) {
            int index = entry.getValue();
            var actor = actors.get(index);
            actors.set(
                    index,
                    new ZLinkUserSpotAggregateStagingOwner.ActorParticipant(
                            actor.actorId(),
                            actor.actorType(),
                            actor.state(),
                            actor.restoreSnapshot(),
                            actor.preparedActorRef(),
                            ZLinkSpotTimerRelocationEnvelope.encodeCanonical(
                                    timersByParticipant.getOrDefault(entry.getKey(), List.of()))));
        }
        List<ZLinkSpotTimerRelocationEnvelope.CanonicalTimer> timers =
                timersByParticipant.getOrDefault(spotParticipant, List.of());
        Class<? extends ZLinkSpot<?>> spotType = spotTypes.apply(stage.stableType());
        if (spotType == null) {
            throw invalid("target does not register User Spot stable type");
        }
        LinkedHashMap<String, List<ZLinkSerialExecutionQueue.QueuedRecord>> frozen =
                new LinkedHashMap<>();
        journal.forEach((lane, records) -> frozen.put(lane, List.copyOf(records)));
        return new ZLinkUserSpotAggregateStagingOwner.Request(
                spotType,
                stage.stableType(),
                stage.spotId(),
                spotGeneration,
                spotState,
                stage.restoreSpotSnapshot(),
                ZLinkSpotTimerRelocationEnvelope.encodeCanonical(timers),
                actors,
                frozen);
    }

    private static long participantId(
            List<ZLinkSpotRetireControl.ParticipantFence> inventory, String lane) {
        String actorId = lane.startsWith("actor:") ? lane.substring(6) : null;
        for (int index = 0; index < inventory.size(); index++) {
            var value = inventory.get(index);
            if (actorId == null && value.objectKind() == 2
                    || actorId != null
                            && value.objectKind() == 1
                            && value.objectId().equals(actorId)) {
                return index + 1L;
            }
        }
        throw invalid("accepted journal lane is outside participant inventory");
    }

    private static IllegalArgumentException invalid(String message) {
        return new IllegalArgumentException(message);
    }

    private record Journal(long participantId, long sequence, byte[] record) {
        private Journal {
            record = record.clone();
        }
    }

    private record PendingTimer(
            long participantId,
            long sequence,
            ZLinkSpotTimerRelocationEnvelope.CanonicalTimer timer) {}

    private record ParticipantTimer(
            long participantId, ZLinkSpotTimerRelocationEnvelope.CanonicalTimer timer) {}

    private record TimerKey(long participantId, String timerName) {}
}

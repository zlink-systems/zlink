package systems.zlink.framework.runtime.internal.service;

import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.ActorRef;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.ApplicationVersion;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.Bool8;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.DecoderContext;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.DurableStateBlob;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.NonzeroU64;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.OrdinalOrZero;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.RelocationApplicationStateFalse;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.RelocationApplicationStateTrue;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.RelocationEnvelopeV1;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.RelocationId;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.RelocationObjectIdentity;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.RelocationObjectIdentityActor;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.RelocationObjectIdentityInstanceSpot;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.RelocationObjectIdentityUserSpot;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.RelocationParticipantApplicationState;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.RelocationParticipantApplicationStateVector;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.RelocationPendingTimerTick;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.RelocationPendingTimerTickVector;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.RelocationSessionBinding;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.RelocationSessionBindingVector;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.RelocationTimerRegistration;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.RelocationTimerRegistrationVector;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.Rid;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.SavedWorkVector;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.SpotRef;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.StatefulObjectKind;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.Text8;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.TimerOverrunPolicyKind;
import systems.zlink.framework.runtime.internal.service.ServiceWireCodec.U64;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Objects;

/** Adapts the generated {@code relocation-envelope-v1} codec to the runtime relocation model. */
public final class ZLinkServiceRelocationEnvelopeCodec {
    private ZLinkServiceRelocationEnvelopeCodec() {}

    public static Envelope decode(byte[] encoded) {
        Objects.requireNonNull(encoded, "encoded");
        try {
            var context = ZLinkServiceM6BWireCodec.generatedContext();
            var generated = ServiceWireCodec.decodeLogicalRelocationEnvelopeV1(encoded, context);
            var states = generated.applicationStates().items();
            var timers = generated.timerRegistrations().items();
            var pending = generated.pendingTimerTicks().items();
            var work = generated.savedWork().items();
            validateReferences(states, work, timers, pending);
            return new Envelope(
                    generated.relocation().high().value(),
                    generated.relocation().low().value(),
                    objectIdentity(generated.object()),
                    generated.applicationVersion().value(),
                    applicationStates(states),
                    savedWork(work, context),
                    timerRegistrations(timers),
                    pendingTimerTicks(pending),
                    generated.sessionBindings().items().stream()
                            .map(ZLinkServiceRelocationEnvelopeCodec::sessionBinding)
                            .toList(),
                    encoded);
        } catch (IOException failure) {
            throw invalid("envelope", failure);
        }
    }

    public static byte[] encode(ServiceWirePilotCodec.RelocationEnvelopeV1 envelope)
            throws IOException {
        var context = ZLinkServiceM6BWireCodec.generatedContext();
        List<ServiceWireCodec.SavedWorkEntry> work = new ArrayList<>(envelope.savedWork().size());
        for (var entry : envelope.savedWork()) {
            var record = ServiceWireCodec.decodeFrozenRecord(entry.frozenRecord(), context);
            ZLinkServiceFrozenRecordCodec.validateInstanceOperation(record);
            work.add(
                    new ServiceWireCodec.SavedWorkEntry(
                            new NonzeroU64(entry.participantId()),
                            new NonzeroU64(entry.order()),
                            record));
        }
        return ServiceWireCodec.encodeLogicalRelocationEnvelopeV1(
                new RelocationEnvelopeV1(
                        new RelocationId(
                                new U64(envelope.relocationHigh()),
                                new U64(envelope.relocationLow())),
                        generatedObject(envelope.object()),
                        new ApplicationVersion(envelope.applicationVersion()),
                        new RelocationParticipantApplicationStateVector(
                                envelope.applicationStates().stream()
                                        .map(
                                                state ->
                                                        new RelocationParticipantApplicationState(
                                                                new NonzeroU64(
                                                                        state.participantId()),
                                                                state.hasState()
                                                                        ? new RelocationApplicationStateTrue(
                                                                                Bool8.TRUE,
                                                                                new DurableStateBlob(
                                                                                        state
                                                                                                .payload()))
                                                                        : new RelocationApplicationStateFalse(
                                                                                Bool8.FALSE)))
                                        .toList()),
                        new SavedWorkVector(work),
                        new RelocationTimerRegistrationVector(
                                envelope.timerRegistrations().stream()
                                        .map(ZLinkServiceRelocationEnvelopeCodec::generatedTimer)
                                        .toList()),
                        new RelocationPendingTimerTickVector(
                                envelope.pendingTimerTicks().stream()
                                        .map(
                                                tick ->
                                                        new RelocationPendingTimerTick(
                                                                new NonzeroU64(
                                                                        tick.participantId()),
                                                                new NonzeroU64(tick.order()),
                                                                new Text8(tick.timerName()),
                                                                new NonzeroU64(
                                                                        tick.deliveryIndex()),
                                                                new NonzeroU64(
                                                                        tick.scheduledIndex()),
                                                                new U64(
                                                                        tick
                                                                                .scheduledAtUnixMilliseconds()),
                                                                new OrdinalOrZero(
                                                                        tick.skippedTicks())))
                                        .toList()),
                        new RelocationSessionBindingVector(
                                envelope.sessionBindings().stream()
                                        .map(
                                                binding ->
                                                        new RelocationSessionBinding(
                                                                new NonzeroU64(
                                                                        binding.participantId()),
                                                                new Rid(
                                                                        binding
                                                                                .sessionOwnerNodeRid()),
                                                                new NonzeroU64(
                                                                        binding
                                                                                .sessionOwnerNodeGeneration()),
                                                                new Text8(binding.sessionOwnerId()),
                                                                new NonzeroU64(
                                                                        binding
                                                                                .sessionOwnerLeaseGeneration()),
                                                                new Rid(binding.sessionRid()),
                                                                new NonzeroU64(
                                                                        binding
                                                                                .bindingGeneration())))
                                        .toList())),
                context);
    }

    private static void validateReferences(
            List<RelocationParticipantApplicationState> states,
            List<ServiceWireCodec.SavedWorkEntry> work,
            List<RelocationTimerRegistration> timers,
            List<RelocationPendingTimerTick> pending) {
        var participants = new HashSet<Long>();
        states.forEach(state -> participants.add(state.participantId().value()));
        var registered = new HashSet<TimerKey>();
        for (var timer : timers) {
            if (!participants.contains(timer.participantId().value())) {
                throw invalid("timer participant");
            }
            registered.add(new TimerKey(timer.participantId().value(), timer.name().value()));
        }
        for (var record : work) {
            if (!participants.contains(record.participantId().value())) {
                throw invalid("saved work participant");
            }
            ZLinkServiceFrozenRecordCodec.validateInstanceOperation(record.recordValue());
        }
        for (var tick : pending) {
            if (!registered.contains(
                    new TimerKey(tick.participantId().value(), tick.timerName().value()))) {
                throw invalid("pending timer registration");
            }
        }
    }

    private record TimerKey(long participantId, String name) {}

    private static RelocationObjectIdentity generatedObject(
            ServiceWirePilotCodec.RelocationObjectIdentity value) {
        return switch (value) {
            case ServiceWirePilotCodec.RelocationActorIdentity actor ->
                    new RelocationObjectIdentityActor(
                            StatefulObjectKind.ACTOR,
                            new ActorRef(
                                    new Text8(actor.actorId()),
                                    new NonzeroU64(actor.objectGeneration())),
                            new NonzeroU64(actor.expectedAuthorityOwnerGeneration()));
            case ServiceWirePilotCodec.RelocationUserSpotIdentity spot ->
                    new RelocationObjectIdentityUserSpot(
                            StatefulObjectKind.USER_SPOT,
                            new SpotRef(
                                    new Text8(spot.spotId()),
                                    new NonzeroU64(spot.objectGeneration())),
                            new NonzeroU64(spot.expectedAuthorityOwnerGeneration()));
            case ServiceWirePilotCodec.RelocationInstanceSpotIdentity instance ->
                    new RelocationObjectIdentityInstanceSpot(
                            StatefulObjectKind.INSTANCE_SPOT,
                            new Text8(instance.instanceType()),
                            new Text8(instance.spotId()),
                            new NonzeroU64(instance.objectGeneration()));
        };
    }

    private static ObjectIdentity objectIdentity(RelocationObjectIdentity value) {
        return switch (value) {
            case RelocationObjectIdentityActor actor ->
                    new ObjectIdentity(
                            (int) actor.objectKind().wire,
                            actor.actor().actorId().value(),
                            actor.actor().objectGeneration().value(),
                            actor.expectedAuthorityOwnerGeneration().value());
            case RelocationObjectIdentityUserSpot spot ->
                    new ObjectIdentity(
                            (int) spot.objectKind().wire,
                            spot.spot().spotId().value(),
                            spot.spot().objectGeneration().value(),
                            spot.expectedAuthorityOwnerGeneration().value());
            case RelocationObjectIdentityInstanceSpot instance ->
                    new ObjectIdentity(
                            (int) instance.objectKind().wire,
                            instance.spotId().value(),
                            instance.objectGeneration().value(),
                            0,
                            instance.instanceType().value());
        };
    }

    private static List<ApplicationState> applicationStates(
            List<RelocationParticipantApplicationState> values) {
        return values.stream()
                .map(
                        value -> {
                            if (value.applicationState()
                                    instanceof RelocationApplicationStateTrue state) {
                                return new ApplicationState(
                                        value.participantId().value(),
                                        true,
                                        state.payload().value());
                            }
                            return new ApplicationState(
                                    value.participantId().value(), false, new byte[0]);
                        })
                .toList();
    }

    private static List<SavedWorkEntry> savedWork(
            List<ServiceWireCodec.SavedWorkEntry> values, DecoderContext context)
            throws IOException {
        List<SavedWorkEntry> result = new ArrayList<>(values.size());
        for (var value : values) {
            long participant = value.participantId().value();
            long order = value.order().value();
            byte[] frozenRecord = ServiceWireCodec.encodeFrozenRecord(value.recordValue(), context);
            byte[] rawEntry =
                    ByteBuffer.allocate(16 + frozenRecord.length)
                            .order(ByteOrder.BIG_ENDIAN)
                            .putLong(participant)
                            .putLong(order)
                            .put(frozenRecord)
                            .array();
            result.add(new SavedWorkEntry(participant, order, rawEntry));
        }
        return result;
    }

    private static RelocationTimerRegistration generatedTimer(
            ServiceWirePilotCodec.RelocationTimerRegistration value) {
        return new RelocationTimerRegistration(
                new NonzeroU64(value.participantId()),
                new Text8(value.name()),
                new Text8(value.handlerType()),
                new NonzeroU64(value.periodMilliseconds()),
                Arrays.stream(TimerOverrunPolicyKind.values())
                        .filter(policy -> policy.wire == value.overrunPolicy())
                        .findFirst()
                        .orElseThrow(() -> invalid("timer overrun policy")),
                new NonzeroU64(value.maxCatchUpTicks()),
                value.stopOnUnhandledException() ? Bool8.TRUE : Bool8.FALSE,
                new OrdinalOrZero(value.lastCompletedDeliveryIndex()),
                new OrdinalOrZero(value.lastCompletedScheduledIndex()),
                new U64(value.nextScheduledAtUnixMilliseconds()));
    }

    private static List<TimerRegistration> timerRegistrations(
            List<RelocationTimerRegistration> values) {
        return values.stream()
                .map(
                        value ->
                                new TimerRegistration(
                                        value.participantId().value(),
                                        value.name().value(),
                                        value.handlerType().value(),
                                        value.periodMilliseconds().value(),
                                        (int) value.overrunPolicy().wire,
                                        value.maxCatchUpTicks().value(),
                                        value.stopOnUnhandledException() == Bool8.TRUE,
                                        value.lastCompletedDeliveryIndex().value(),
                                        value.lastCompletedScheduledIndex().value(),
                                        value.nextScheduledAtUnixMilliseconds().value()))
                .toList();
    }

    private static List<PendingTimerTick> pendingTimerTicks(
            List<RelocationPendingTimerTick> values) {
        return values.stream()
                .map(
                        value ->
                                new PendingTimerTick(
                                        value.participantId().value(),
                                        value.order().value(),
                                        value.timerName().value(),
                                        value.deliveryIndex().value(),
                                        value.scheduledIndex().value(),
                                        value.scheduledAtUnixMilliseconds().value(),
                                        value.skippedTicks().value()))
                .toList();
    }

    private static ServiceWirePilotCodec.RelocationSessionBinding sessionBinding(
            RelocationSessionBinding value) {
        return new ServiceWirePilotCodec.RelocationSessionBinding(
                value.participantId().value(),
                value.sessionOwnerNodeRid().value(),
                value.sessionOwnerNodeGeneration().value(),
                value.sessionOwnerId().value(),
                value.sessionOwnerLeaseGeneration().value(),
                value.sessionRid().value(),
                value.bindingGeneration().value());
    }

    public record Envelope(
            long relocationHigh,
            long relocationLow,
            ObjectIdentity object,
            long applicationVersion,
            List<ApplicationState> applicationStates,
            List<SavedWorkEntry> savedWork,
            List<TimerRegistration> timerRegistrations,
            List<PendingTimerTick> pendingTimerTicks,
            List<ServiceWirePilotCodec.RelocationSessionBinding> sessionBindings,
            byte[] canonicalBytes) {
        public Envelope {
            applicationStates = List.copyOf(applicationStates);
            savedWork = List.copyOf(savedWork);
            timerRegistrations = List.copyOf(timerRegistrations);
            pendingTimerTicks = List.copyOf(pendingTimerTicks);
            sessionBindings = List.copyOf(sessionBindings);
            canonicalBytes = canonicalBytes.clone();
        }

        @Override
        public byte[] canonicalBytes() {
            return canonicalBytes.clone();
        }
    }

    public record SavedWorkEntry(long participantId, long sequence, byte[] rawEntry) {
        public SavedWorkEntry {
            rawEntry = rawEntry.clone();
        }

        @Override
        public byte[] rawEntry() {
            return rawEntry.clone();
        }

        /**
         * Returns only the canonical frozen operation. The raw entry also contains the participant
         * and sequence prefixes used when the durable envelope is rewritten.
         */
        public byte[] frozenRecord() {
            if (rawEntry.length <= 16) {
                throw new IllegalStateException(
                        "saved-work entry does not contain a frozen operation");
            }
            return Arrays.copyOfRange(rawEntry, 16, rawEntry.length);
        }
    }

    public record ObjectIdentity(
            int kind,
            String objectId,
            long objectGeneration,
            long expectedAuthorityOwnerGeneration,
            String stableType) {
        public ObjectIdentity(
                int kind,
                String objectId,
                long objectGeneration,
                long expectedAuthorityOwnerGeneration) {
            this(kind, objectId, objectGeneration, expectedAuthorityOwnerGeneration, "");
        }
    }

    public record ApplicationState(long participantId, boolean hasState, byte[] payload) {
        public ApplicationState {
            payload = payload.clone();
        }

        @Override
        public byte[] payload() {
            return payload.clone();
        }
    }

    public record TimerRegistration(
            long participantId,
            String name,
            String handlerType,
            long periodMilliseconds,
            int overrunPolicy,
            long maxCatchUpTicks,
            boolean stopOnUnhandledException,
            long lastCompletedDeliveryIndex,
            long lastCompletedScheduledIndex,
            long nextScheduledAtUnixMilliseconds) {}

    public record PendingTimerTick(
            long participantId,
            long sequence,
            String timerName,
            long deliveryIndex,
            long scheduledIndex,
            long scheduledAtUnixMilliseconds,
            long skippedTicks) {}

    private static IllegalArgumentException invalid(String label) {
        return new IllegalArgumentException("Invalid canonical relocation " + label);
    }

    private static IllegalArgumentException invalid(String label, Throwable failure) {
        return new IllegalArgumentException("Invalid canonical relocation " + label, failure);
    }
}

package systems.zlink.framework.runtime.internal.locations;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Objects;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;

/**
 * Adapts the generated {@code relocation-envelope-v1} codec to the runtime
 * relocation model.
 */
public final class ZLinkServiceRelocationEnvelopeCodec {
    private ZLinkServiceRelocationEnvelopeCodec() {
    }

    public static Envelope decode(byte[] encoded) {
        Objects.requireNonNull(encoded, "encoded");
        try {
            var generated = ServiceWirePilotCodec.decodeRelocationEnvelopeV1(
                List.of(encoded));
            byte[] canonical = ServiceWirePilotCodec.encodeRelocationEnvelopeV1(
                generated);
            return new Envelope(
                generated.relocationHigh(),
                generated.relocationLow(),
                objectIdentity(generated.object()),
                generated.applicationVersion(),
                applicationStates(generated.applicationStates()),
                savedWork(generated.savedWork()),
                timerRegistrations(generated.timerRegistrations()),
                pendingTimerTicks(generated.pendingTimerTicks()),
                canonical);
        } catch (IOException failure) {
            throw invalid("envelope", failure);
        }
    }

    private static ObjectIdentity objectIdentity(
        ServiceWirePilotCodec.RelocationObjectIdentity value) {
        if (value instanceof ServiceWirePilotCodec.RelocationActorIdentity actor) {
            return new ObjectIdentity(
                1, actor.actorId(), actor.objectGeneration(),
                actor.expectedAuthorityOwnerGeneration());
        }
        if (value instanceof ServiceWirePilotCodec.RelocationUserSpotIdentity spot) {
            return new ObjectIdentity(
                2, spot.spotId(), spot.objectGeneration(),
                spot.expectedAuthorityOwnerGeneration());
        }
        if (value instanceof ServiceWirePilotCodec.RelocationInstanceSpotIdentity
            instance) {
            return new ObjectIdentity(
                3, instance.spotId(), instance.objectGeneration(), 0);
        }
        throw invalid("object identity");
    }

    private static List<ApplicationState> applicationStates(
        List<ServiceWirePilotCodec.RelocationApplicationState> values) {
        List<ApplicationState> result = new ArrayList<>(values.size());
        for (var value : values) {
            result.add(new ApplicationState(
                value.participantId(), value.hasState(), value.payload()));
        }
        return result;
    }

    private static List<SavedWorkEntry> savedWork(
        List<ServiceWirePilotCodec.RelocationSavedWork> values) {
        List<SavedWorkEntry> result = new ArrayList<>(values.size());
        for (var value : values) {
            byte[] frozenRecord = value.frozenRecord();
            byte[] rawEntry = ByteBuffer.allocate(16 + frozenRecord.length)
                .order(ByteOrder.BIG_ENDIAN)
                .putLong(value.participantId())
                .putLong(value.order())
                .put(frozenRecord)
                .array();
            result.add(new SavedWorkEntry(
                value.participantId(), value.order(), rawEntry));
        }
        return result;
    }

    private static List<TimerRegistration> timerRegistrations(
        List<ServiceWirePilotCodec.RelocationTimerRegistration> values) {
        List<TimerRegistration> result = new ArrayList<>(values.size());
        for (var value : values) {
            result.add(new TimerRegistration(
                value.participantId(), value.name(), value.handlerType(),
                value.periodMilliseconds(), value.overrunPolicy(),
                value.maxCatchUpTicks(), value.stopOnUnhandledException(),
                value.lastCompletedDeliveryIndex(),
                value.lastCompletedScheduledIndex(),
                value.nextScheduledAtUnixMilliseconds()));
        }
        return result;
    }

    private static List<PendingTimerTick> pendingTimerTicks(
        List<ServiceWirePilotCodec.RelocationPendingTimerTick> values) {
        List<PendingTimerTick> result = new ArrayList<>(values.size());
        for (var value : values) {
            result.add(new PendingTimerTick(
                value.participantId(), value.order(), value.timerName(),
                value.deliveryIndex(), value.scheduledIndex(),
                value.scheduledAtUnixMilliseconds(), value.skippedTicks()));
        }
        return result;
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
        byte[] canonicalBytes) {
        public Envelope {
            applicationStates = List.copyOf(applicationStates);
            savedWork = List.copyOf(savedWork);
            timerRegistrations = List.copyOf(timerRegistrations);
            pendingTimerTicks = List.copyOf(pendingTimerTicks);
            canonicalBytes = canonicalBytes.clone();
        }

        @Override public byte[] canonicalBytes() { return canonicalBytes.clone(); }
    }

    public record SavedWorkEntry(
        long participantId,
        long sequence,
        byte[] rawEntry) {
        public SavedWorkEntry {
            rawEntry = rawEntry.clone();
        }

        @Override public byte[] rawEntry() { return rawEntry.clone(); }

        /**
         * Returns only the canonical frozen operation. The raw entry also
         * contains the participant and sequence prefixes used when the
         * durable envelope is rewritten.
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
        long expectedAuthorityOwnerGeneration) {
    }

    public record ApplicationState(
        long participantId,
        boolean hasState,
        byte[] payload) {
        public ApplicationState { payload = payload.clone(); }
        @Override public byte[] payload() { return payload.clone(); }
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
        long nextScheduledAtUnixMilliseconds) {
    }

    public record PendingTimerTick(
        long participantId,
        long sequence,
        String timerName,
        long deliveryIndex,
        long scheduledIndex,
        long scheduledAtUnixMilliseconds,
        long skippedTicks) {
    }

    private static IllegalArgumentException invalid(String label) {
        return new IllegalArgumentException(
            "Invalid canonical relocation " + label);
    }

    private static IllegalArgumentException invalid(
        String label,
        Throwable failure) {
        return new IllegalArgumentException(
            "Invalid canonical relocation " + label, failure);
    }
}

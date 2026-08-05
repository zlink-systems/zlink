package systems.zlink.framework.runtime.spots;

import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Objects;
import java.util.UUID;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkServiceRelocationEnvelopeCodec;

/** Projects one independent Actor onto relocation-envelope-v1. */
public final class ZLinkCanonicalActorRelocationEnvelope {
    private ZLinkCanonicalActorRelocationEnvelope() {
    }

    public static byte[] encode(
        UUID relocationId,
        String actorId,
        long objectGeneration,
        long expectedAuthorityOwnerGeneration,
        boolean restoreSnapshot,
        byte[] state,
        List<ZLinkAsyncSerialQueue.QueuedRecord> journal) {
        return encode(
            relocationId,
            actorId,
            objectGeneration,
            expectedAuthorityOwnerGeneration,
            restoreSnapshot,
            state,
            journal,
            ZLinkSpotTimerRelocationEnvelope.encodeCanonical(List.of()));
    }

    static byte[] encode(
        UUID relocationId,
        String actorId,
        long objectGeneration,
        long expectedAuthorityOwnerGeneration,
        boolean restoreSnapshot,
        byte[] state,
        List<ZLinkAsyncSerialQueue.QueuedRecord> journal,
        byte[] timerEnvelope) {
        Objects.requireNonNull(relocationId, "relocationId");
        Objects.requireNonNull(actorId, "actorId");
        byte[] applicationState = Objects.requireNonNull(state, "state").clone();
        List<ZLinkAsyncSerialQueue.QueuedRecord> records =
            new ArrayList<>(Objects.requireNonNull(journal, "journal"));
        records.sort(Comparator.comparingLong(
            ZLinkAsyncSerialQueue.QueuedRecord::sequence));
        long previous = 0;
        for (var record : records) {
            if (Long.compareUnsigned(record.sequence(), previous) <= 0) {
                throw new IllegalArgumentException(
                    "Actor journal sequence is not canonical");
            }
            previous = record.sequence();
        }
        List<ZLinkSpotTimerRelocationEnvelope.CanonicalTimer> timers =
            new ArrayList<>(
                ZLinkSpotTimerRelocationEnvelope.canonicalize(
                    timerEnvelope));
        timers.sort(Comparator.comparing(
            ZLinkSpotTimerRelocationEnvelope.CanonicalTimer::name));
        long acceptedBoundary = records.isEmpty()
            ? 0 : records.getLast().sequence();
        List<PendingTimer> pendingTimers = new ArrayList<>();
        for (var timer : timers) {
            if (timer.pending() != null) {
                acceptedBoundary = Math.incrementExact(acceptedBoundary);
                pendingTimers.add(new PendingTimer(
                    acceptedBoundary, timer));
            }
        }

        Writer writer = new Writer();
        writer.u64(relocationId.getMostSignificantBits());
        writer.u64(relocationId.getLeastSignificantBits());
        Writer object = new Writer();
        object.text8(actorId);
        object.u64(objectGeneration);
        object.u64(expectedAuthorityOwnerGeneration);
        writer.u8(1);
        writer.body16(object);
        writer.u64(1);
        writer.u32(1);
        writer.u64(1);
        writer.u8(restoreSnapshot ? 1 : 0);
        Writer stateBody = new Writer();
        if (restoreSnapshot) {
            stateBody.bytes64(applicationState);
        }
        writer.body64(stateBody);
        writer.u32(1);
        writer.u64(1);
        writer.u64(acceptedBoundary);
        writer.u64(0);
        writer.u32(records.size());
        for (var record : records) {
            writer.u64(1);
            writer.u64(record.sequence());
            writer.raw(record.payload());
        }
        writer.u32(timers.size());
        for (var timer : timers) {
            writer.u64(1);
            writer.text8(timer.name());
            writer.text8(timer.handlerType());
            writer.u64(timer.periodMilliseconds());
            writer.u8(timer.overrunPolicy());
            writer.u64(timer.maxCatchUpTicks());
            writer.u8(timer.stopOnUnhandledException() ? 1 : 0);
            writer.u64(timer.lastCompletedDeliveryIndex());
            writer.u64(timer.lastCompletedScheduledIndex());
            writer.u64(timer.nextScheduledAtUnixMilliseconds());
        }
        writer.u32(pendingTimers.size());
        for (PendingTimer pending : pendingTimers) {
            var timer = pending.timer();
            var tick = timer.pending();
            writer.u64(1);
            writer.u64(pending.sequence());
            writer.text8(timer.name());
            writer.u64(tick.deliveryIndex());
            writer.u64(tick.scheduledIndex());
            writer.u64(tick.scheduledAtUnixMilliseconds());
            writer.u64(tick.skippedTicks());
        }
        writer.u32(0);
        byte[] encoded = writer.bytes();
        ZLinkServiceRelocationEnvelopeCodec.decode(encoded);
        return encoded;
    }

    static Decoded decode(
        byte[] encoded,
        UUID relocationId,
        String actorId,
        boolean restoreSnapshot) {
        var root = ZLinkServiceRelocationEnvelopeCodec.decode(encoded);
        if (root.relocationHigh() != relocationId.getMostSignificantBits()
            || root.relocationLow() != relocationId.getLeastSignificantBits()
            || root.object().kind() != 1
            || !root.object().objectId().equals(actorId)
            || root.applicationStates().size() != 1
            || root.participantProgress().size() != 1) {
            throw new IllegalArgumentException(
                "canonical Actor relocation identity or inventory differs");
        }
        var state = root.applicationStates().getFirst();
        var progress = root.participantProgress().getFirst();
        if (state.participantId() != 1
            || state.hasState() != restoreSnapshot
            || progress.participantId() != 1) {
            throw new IllegalArgumentException(
                "canonical Actor relocation state policy differs");
        }
        List<ZLinkAsyncSerialQueue.QueuedRecord> journal =
            root.journal().stream()
                .map(value -> {
                    if (value.participantId() != 1) {
                        throw new IllegalArgumentException(
                            "Actor journal references another participant");
                    }
                    return new ZLinkAsyncSerialQueue.QueuedRecord(
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
                    .map(registration -> {
                        var pending = root.pendingTimerTicks().stream()
                            .filter(value ->
                                value.participantId() == 1
                                    && value.timerName().equals(
                                        registration.name()))
                            .findFirst()
                            .map(value ->
                                new ZLinkSpotTimerRelocationEnvelope
                                    .CanonicalPending(
                                        value.deliveryIndex(),
                                        value.scheduledIndex(),
                                        value.scheduledAtUnixMilliseconds(),
                                        value.skippedTicks()))
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
                                registration
                                    .nextScheduledAtUnixMilliseconds(),
                                pending);
                    })
                    .toList()));
    }

    record Decoded(
        long objectGeneration,
        long expectedAuthorityOwnerGeneration,
        byte[] state,
        List<ZLinkAsyncSerialQueue.QueuedRecord> journal,
        byte[] timerEnvelope) {
        Decoded {
            state = state.clone();
            journal = List.copyOf(journal);
            timerEnvelope = timerEnvelope.clone();
        }

        @Override public byte[] state() {
            return state.clone();
        }

        @Override public byte[] timerEnvelope() {
            return timerEnvelope.clone();
        }
    }

    private record PendingTimer(
        long sequence,
        ZLinkSpotTimerRelocationEnvelope.CanonicalTimer timer) {
    }

    private static final class Writer {
        private final ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        private final DataOutputStream output = new DataOutputStream(bytes);

        void u8(long value) {
            write(() -> output.writeByte((int) value));
        }

        void u32(long value) {
            write(() -> output.writeInt((int) value));
        }

        void u64(long value) {
            write(() -> output.writeLong(value));
        }

        void raw(byte[] value) {
            write(() -> output.write(value));
        }

        void text8(String value) {
            byte[] encoded = value.getBytes(java.nio.charset.StandardCharsets.UTF_8);
            if (encoded.length < 1 || encoded.length > 255
                || value.indexOf('\0') >= 0) {
                throw new IllegalArgumentException("text8");
            }
            u8(encoded.length);
            raw(encoded);
        }

        void bytes64(byte[] value) {
            u64(value.length);
            raw(value);
        }

        void body16(Writer body) {
            byte[] encoded = body.bytes();
            if (encoded.length > 0xffff) {
                throw new IllegalArgumentException("body16");
            }
            write(() -> output.writeShort(encoded.length));
            raw(encoded);
        }

        void body64(Writer body) {
            byte[] encoded = body.bytes();
            u64(encoded.length);
            raw(encoded);
        }

        byte[] bytes() {
            return bytes.toByteArray();
        }

        private void write(IoAction action) {
            try {
                action.run();
            } catch (IOException failure) {
                throw new IllegalStateException(failure);
            }
        }
    }

    @FunctionalInterface
    private interface IoAction {
        void run() throws IOException;
    }
}

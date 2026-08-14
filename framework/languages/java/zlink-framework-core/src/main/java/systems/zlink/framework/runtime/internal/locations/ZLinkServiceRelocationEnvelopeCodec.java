package systems.zlink.framework.runtime.internal.locations;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;

/**
 * Decodes the shared immutable {@code relocation-envelope-v1} handoff root.
 */
public final class ZLinkServiceRelocationEnvelopeCodec {
    private static final int MAX_RECORDS = 65_536;

    private ZLinkServiceRelocationEnvelopeCodec() {
    }

    public static Envelope decode(byte[] encoded) {
        Reader reader = new Reader(encoded);
        long relocationHigh = reader.u64();
        long relocationLow = reader.u64();
        if (relocationHigh == 0 && relocationLow == 0) {
            throw invalid("relocation id");
        }
        ObjectIdentity object = readRelocationObject(reader);
        long applicationVersion = reader.i64();
        if (applicationVersion < 0) {
            throw invalid("application version");
        }

        Set<Long> states = new HashSet<>();
        List<ApplicationState> applicationStates = new ArrayList<>();
        int stateCount = reader.count(MAX_RECORDS, "application state");
        if (stateCount > reader.remaining() / 13) {
            throw invalid("application state count exceeds remaining bytes");
        }
        long previousId = 0;
        for (int index = 0; index < stateCount; index++) {
            long participantId = reader.nonzeroU64(
                "application-state participant id");
            if (Long.compareUnsigned(participantId, previousId) <= 0) {
                throw invalid("application-state participant order");
            }
            previousId = participantId;
            states.add(participantId);
            boolean hasState = reader.bool();
            Reader body = reader.body64();
            byte[] payload = new byte[0];
            if (hasState) {
                payload = body.bytes64();
            }
            body.end("application state");
            applicationStates.add(new ApplicationState(
                participantId, hasState, payload));
        }

        List<SavedWorkEntry> savedWork = readSavedWork(reader, states);
        TimerSection timers = readTimerRegistrations(
            reader, states);
        List<PendingTimerTick> pendingTicks = readPendingTimerTicks(
            reader, states, timers.names());
        reader.end("relocation envelope");

        byte[] canonical = Objects.requireNonNull(encoded, "encoded").clone();
        return new Envelope(
            relocationHigh,
            relocationLow,
            object,
            applicationVersion,
            applicationStates,
            savedWork,
            timers.registrations(),
            pendingTicks,
            canonical);
    }

    private static List<SavedWorkEntry> readSavedWork(
        Reader reader,
        Set<Long> participants) {
        int count = reader.count(MAX_RECORDS, "saved work");
        if (count > reader.remaining()) {
            throw invalid("saved work count exceeds remaining bytes");
        }
        List<SavedWorkEntry> entries = new ArrayList<>(count);
        long previousParticipant = 0;
        long previousSequence = 0;
        for (int index = 0; index < count; index++) {
            int entryStart = reader.position();
            long participantId = reader.nonzeroU64("saved-work participant id");
            long sequence = reader.nonzeroU64("saved-work order");
            if (!participants.contains(participantId)
                || Long.compareUnsigned(participantId, previousParticipant) < 0
                || participantId == previousParticipant
                    && Long.compareUnsigned(sequence, previousSequence) <= 0) {
                throw invalid("saved work order");
            }
            previousParticipant = participantId;
            previousSequence = sequence;
            readFrozenRecord(reader);
            entries.add(new SavedWorkEntry(
                participantId,
                sequence,
                reader.copy(entryStart, reader.position())));
        }
        return entries;
    }

    private static void readFrozenRecord(Reader reader) {
        int kind = reader.u8();
        if (kind < 1 || kind > 14) {
            throw invalid("journal record kind");
        }
        int sourceKind = reader.u8();
        Reader source = reader.body16();
        source.text8();
        source.nonzeroU64("source node generation");
        source.text8();
        source.nonzeroU64("source owner lease generation");
        if (sourceKind == 2) {
            source.text8();
        } else if (sourceKind == 3) {
            readActorRef(source);
        } else if (sourceKind == 4) {
            readActorRef(source);
            source.text8();
            source.nonzeroU64("source binding generation");
            source.nonzeroU64("source session sequence");
        } else if (sourceKind != 1) {
            throw invalid("journal source kind");
        }
        source.end("journal source");
        if (reader.bool()) {
            readMetadata(reader);
        }
        reader.u64();
        reader.u64();
        long operationKind = Integer.toUnsignedLong(reader.u32());
        Reader reply = reader.body16();
        if (operationKind == 1 || operationKind == 2 || operationKind == 3
            || operationKind == 4 || operationKind == 12) {
            reply.nonzeroU64("reply route id");
        }
        reply.end("journal reply route");
        readFrozenBody(reader, kind);
    }

    private static void readFrozenBody(Reader reader, int kind) {
        if (kind == 1 || kind == 2) {
            readApplicationPayload(reader);
        } else if (kind == 3 || kind == 4) {
            reader.text8();
            readApplicationPayload(reader);
        } else if (kind == 5 || kind == 6) {
            readSpotRouteFence(reader);
            readApplicationPayload(reader);
        } else if (kind == 7) {
            reader.text8();
            reader.text8();
            readApplicationPayload(reader);
        } else if (kind == 9 || kind == 10) {
            readActorRouteFence(reader);
            readApplicationPayload(reader);
        } else if (kind == 11) {
            terminalResult(reader.u32());
            reader.u32();
            if (reader.bool()) {
                readApplicationPayload(reader);
            }
        } else {
            throw invalid("unsupported journal record kind");
        }
    }

    private static TimerSection readTimerRegistrations(
        Reader reader,
        Set<Long> participants) {
        int count = reader.count(MAX_RECORDS, "timer registration");
        if (count > reader.remaining()) {
            throw invalid(
                "timer registration count exceeds remaining bytes");
        }
        Map<Long, Set<String>> names = new HashMap<>();
        List<TimerRegistration> registrations = new ArrayList<>();
        long previousParticipant = 0;
        byte[] previousName = new byte[0];
        for (int index = 0; index < count; index++) {
            long participantId = reader.nonzeroU64("timer participant id");
            String name = reader.text8();
            String handlerType = reader.text8();
            long period = reader.nonzeroU64("timer period");
            int policy = reader.u8();
            long catchUp = reader.nonzeroU64("timer catch-up bound");
            boolean stop = reader.bool();
            long delivery = reader.u64();
            long scheduled = reader.u64();
            long nextAt = reader.u64();
            byte[] encodedName = name.getBytes(StandardCharsets.UTF_8);
            if (!participants.contains(participantId)
                || Long.compareUnsigned(participantId, previousParticipant) < 0
                || participantId == previousParticipant
                    && Arrays.compareUnsigned(encodedName, previousName) <= 0
                || policy < 1 || policy > 3) {
                throw invalid("timer registration order");
            }
            names.computeIfAbsent(participantId, ignored -> new HashSet<>())
                .add(name);
            previousParticipant = participantId;
            previousName = encodedName;
            registrations.add(new TimerRegistration(
                participantId, name, handlerType, period, policy, catchUp,
                stop, delivery, scheduled, nextAt));
        }
        return new TimerSection(names, registrations);
    }

    private static List<PendingTimerTick> readPendingTimerTicks(
        Reader reader,
        Set<Long> participants,
        Map<Long, Set<String>> timerNames) {
        int count = reader.count(MAX_RECORDS, "pending timer tick");
        if (count > reader.remaining()) {
            throw invalid(
                "pending timer tick count exceeds remaining bytes");
        }
        List<PendingTimerTick> values = new ArrayList<>();
        long previousParticipant = 0;
        long previousSequence = 0;
        for (int index = 0; index < count; index++) {
            long participantId = reader.nonzeroU64(
                "pending timer participant id");
            long sequence = reader.nonzeroU64("pending timer sequence");
            String name = reader.text8();
            long delivery = reader.nonzeroU64("timer delivery index");
            long scheduled = reader.nonzeroU64("timer scheduled index");
            long scheduledAt = reader.u64();
            long skipped = reader.u64();
            if (!participants.contains(participantId)
                || !timerNames.getOrDefault(participantId, Set.of())
                    .contains(name)
                || Long.compareUnsigned(participantId, previousParticipant) < 0
                || participantId == previousParticipant
                    && Long.compareUnsigned(sequence, previousSequence) <= 0) {
                throw invalid("pending timer tick order");
            }
            previousParticipant = participantId;
            previousSequence = sequence;
            values.add(new PendingTimerTick(
                participantId, sequence, name, delivery, scheduled,
                scheduledAt, skipped));
        }
        return values;
    }

    private static void readApplicationPayload(Reader reader) {
        if (reader.u8() != 1) {
            throw invalid("application payload version");
        }
        Reader body = reader.body32();
        body.text8();
        body.text8();
        body.bytes32();
        body.end("application payload");
    }

    private static void readMetadata(Reader reader) {
        if (reader.u8() != 1) {
            throw invalid("metadata version");
        }
        int count = reader.u8();
        Set<String> keys = new HashSet<>();
        for (int index = 0; index < count; index++) {
            if (!keys.add(reader.text8())) {
                throw invalid("metadata key");
            }
            reader.text16();
        }
    }

    private static ObjectIdentity readRelocationObject(Reader reader) {
        int kind = reader.u8();
        Reader body = reader.body16();
        String objectId;
        long objectGeneration;
        long ownerGeneration;
        if (kind == 1) {
            objectId = body.text8();
            objectGeneration = body.nonzeroU64("Actor generation");
            ownerGeneration = body.nonzeroU64("Actor owner generation");
        } else if (kind == 2) {
            objectId = body.text8();
            objectGeneration = body.nonzeroU64("Spot generation");
            ownerGeneration = body.nonzeroU64("Spot owner generation");
        } else if (kind == 3) {
            body.text8();
            objectId = body.text8();
            objectGeneration = body.nonzeroU64("Instance generation");
            ownerGeneration = 0;
        } else {
            throw invalid("object kind");
        }
        body.end("relocation object");
        return new ObjectIdentity(
            kind, objectId, objectGeneration, ownerGeneration);
    }

    private static void readActorRef(Reader reader) {
        reader.text8();
        reader.nonzeroU64("Actor generation");
    }

    private static void readSpotRef(Reader reader) {
        reader.text8();
        reader.nonzeroU64("Spot generation");
    }

    private static void readSpotRouteFence(Reader reader) {
        readSpotRef(reader);
        reader.text8();
        reader.nonzeroU64("target node generation");
        reader.nonzeroU64("Spot authority owner generation");
        reader.nonzeroU64("Spot owner lease generation");
    }

    private static void readActorRouteFence(Reader reader) {
        readActorRef(reader);
        reader.text8();
        reader.nonzeroU64("target node generation");
        reader.nonzeroU64("Actor authority owner generation");
        reader.nonzeroU64("Actor owner lease generation");
    }

    private static int terminalResult(int value) {
        long unsigned = Integer.toUnsignedLong(value);
        if (unsigned != 0 && (unsigned < 101 || unsigned > 113)) {
            throw invalid("terminal result");
        }
        return value;
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
            return Arrays.copyOfRange(
                rawEntry, 16, rawEntry.length);
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

    private record TimerSection(
        Map<Long, Set<String>> names,
        List<TimerRegistration> registrations) {
    }

    private static final class Reader {
        private final byte[] source;
        private int offset;

        Reader(byte[] source) {
            this.source = Objects.requireNonNull(source, "source");
        }

        int position() { return offset; }
        byte[] copy(int start, int end) {
            if (start < 0 || end < start || end > source.length) {
                throw invalid("byte range");
            }
            return Arrays.copyOfRange(source, start, end);
        }
        int u8() { return Byte.toUnsignedInt(take(1)[0]); }
        boolean bool() {
            int value = u8();
            if (value > 1) throw invalid("boolean");
            return value == 1;
        }
        int u16() {
            return Short.toUnsignedInt(ByteBuffer.wrap(take(2))
                .order(ByteOrder.BIG_ENDIAN).getShort());
        }
        int u32() {
            return ByteBuffer.wrap(take(4)).order(ByteOrder.BIG_ENDIAN).getInt();
        }
        long u64() {
            return ByteBuffer.wrap(take(8)).order(ByteOrder.BIG_ENDIAN).getLong();
        }
        long i64() { return u64(); }
        long nonzeroU64(String label) {
            long value = u64();
            if (value == 0) throw invalid(label);
            return value;
        }
        int count(int maximum, String label) {
            long value = Integer.toUnsignedLong(u32());
            if (value > maximum) {
                throw new IllegalArgumentException(
                    "Relocation " + label + " count exceeds its bound");
            }
            return (int) value;
        }
        String text8() { return text(u8()); }
        String text16() { return text(u16()); }
        byte[] bytes32() {
            long size = Integer.toUnsignedLong(u32());
            if (size > Integer.MAX_VALUE) throw invalid("byte length");
            return take((int) size);
        }
        byte[] bytes64() {
            long size = u64();
            if (size < 0 || size > Integer.MAX_VALUE) throw invalid("byte length");
            return take((int) size);
        }
        Reader body16() { return new Reader(take(u16())); }
        Reader body32() {
            long size = Integer.toUnsignedLong(u32());
            if (size > Integer.MAX_VALUE) throw invalid("body length");
            return new Reader(take((int) size));
        }
        Reader body64() {
            long size = u64();
            if (size < 0 || size > Integer.MAX_VALUE) throw invalid("body length");
            return new Reader(take((int) size));
        }
        void end(String label) {
            if (offset != source.length) {
                throw new IllegalArgumentException(
                    "Canonical " + label + " contains trailing bytes");
            }
        }
        private String text(int size) {
            if (size == 0) throw invalid("text");
            try {
                return StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(ByteBuffer.wrap(take(size))).toString();
            } catch (CharacterCodingException failure) {
                throw invalid("text", failure);
            }
        }
        private byte[] take(int size) {
            if (size < 0 || size > source.length - offset) {
                throw invalid("truncated envelope");
            }
            byte[] result = Arrays.copyOfRange(source, offset, offset + size);
            offset += size;
            return result;
        }
        int remaining() { return source.length - offset; }
    }

    private static IllegalArgumentException invalid(String field) {
        return new IllegalArgumentException(
            "Invalid canonical relocation " + field);
    }

    private static IllegalArgumentException invalid(
        String field,
        Throwable cause) {
        return new IllegalArgumentException(
            "Invalid canonical relocation " + field,
            cause);
    }
}

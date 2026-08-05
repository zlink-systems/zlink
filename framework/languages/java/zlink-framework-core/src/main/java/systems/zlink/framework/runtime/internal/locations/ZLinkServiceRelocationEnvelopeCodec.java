package systems.zlink.framework.runtime.internal.locations;

import java.io.ByteArrayOutputStream;
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
import systems.zlink.contracts.core.RoutingId;

/**
 * Decodes the shared {@code relocation-envelope-v1} logical stream and emits
 * immutable successor roots. Journal and timer bytes remain byte-identical;
 * only durable replay progress and terminal delivery state may change.
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

        int progressOffset = reader.position();
        int progressCount = reader.count(
            MAX_RECORDS, "participant progress");
        if (progressCount > reader.remaining() / 24) {
            throw invalid("participant progress count exceeds remaining bytes");
        }
        if (progressCount != stateCount) {
            throw invalid("participant progress coverage");
        }
        List<Progress> progress = new ArrayList<>(progressCount);
        Map<Long, Progress> byParticipant = new HashMap<>();
        previousId = 0;
        for (int index = 0; index < progressCount; index++) {
            long participantId = reader.nonzeroU64("progress participant id");
            long acceptedBoundary = reader.u64();
            long replayCursor = reader.u64();
            if (Long.compareUnsigned(participantId, previousId) <= 0
                || !states.contains(participantId)
                || Long.compareUnsigned(replayCursor, acceptedBoundary) > 0) {
                throw invalid("participant progress");
            }
            previousId = participantId;
            Progress value = new Progress(
                participantId, acceptedBoundary, replayCursor);
            progress.add(value);
            byParticipant.put(participantId, value);
        }
        List<JournalEntry> journal = readJournal(reader, byParticipant);
        int journalEnd = reader.position();
        TimerSection timers = readTimerRegistrations(
            reader, states);
        List<PendingTimerTick> pendingTicks = readPendingTimerTicks(
            reader, states, timers.names());
        int completionOffset = reader.position();
        List<Completion> completions = readTerminalCompletions(reader, states);
        reader.end("relocation envelope");

        byte[] canonical = Objects.requireNonNull(encoded, "encoded").clone();
        return new Envelope(
            relocationHigh,
            relocationLow,
            object,
            applicationVersion,
            applicationStates,
            progress,
            completions,
            timers.registrations(),
            pendingTicks,
            canonical,
            Arrays.copyOfRange(canonical, 0, progressOffset),
            journal,
            Arrays.copyOfRange(canonical, journalEnd, completionOffset));
    }

    public static byte[] encodeSuccessor(
        Envelope envelope,
        List<Progress> progress,
        List<Completion> completions) {
        Objects.requireNonNull(envelope, "envelope");
        Writer writer = new Writer();
        writer.bytes(envelope.canonicalPrefix());
        writer.u32(progress.size());
        for (Progress value : progress) {
            writer.u64(value.participantId());
            writer.u64(value.acceptedBoundary());
            writer.u64(value.replayCursor());
        }
        Map<Long, Progress> progressByParticipant = new HashMap<>();
        progress.forEach(value -> progressByParticipant.put(
            value.participantId(), value));
        List<JournalEntry> retained = envelope.journal().stream()
            .filter(entry -> {
                Progress value = progressByParticipant.get(
                    entry.participantId());
                return value != null
                    && Long.compareUnsigned(
                        entry.sequence(), value.replayCursor()) > 0;
            })
            .toList();
        writer.u32(retained.size());
        retained.forEach(value -> writer.bytes(value.rawEntry()));
        writer.bytes(envelope.canonicalAfterJournal());
        writer.u32(completions.size());
        for (Completion value : completions) {
            writer.u64(value.operationHigh());
            writer.u64(value.operationLow());
            writer.text8(value.sourceOwnerId());
            writer.u64(value.sourceOwnerLeaseGeneration());
            writer.text8(value.sourceNodeRid());
            writer.u64(value.sourceNodeGeneration());
            writer.u64(value.participantId());
            writer.u64(value.sequence());
            writer.u32(value.terminalResult());
            writer.u32(value.failureCode());
            writer.u8(value.deliveryState());
            writer.u8(value.payload() == null ? 0 : 1);
            if (value.payload() != null) {
                writeApplicationPayload(writer, value.payload());
            }
        }
        byte[] encoded = writer.toByteArray();
        Envelope verified = decode(encoded);
        if (verified.relocationHigh() != envelope.relocationHigh()
            || verified.relocationLow() != envelope.relocationLow()
            || verified.applicationVersion() != envelope.applicationVersion()) {
            throw invalid("successor identity");
        }
        return encoded;
    }

    public static Envelope advanceReplay(
        Envelope envelope,
        long participantId,
        long sequence,
        Completion completion) {
        Objects.requireNonNull(envelope, "envelope");
        List<Progress> progress = envelope.participantProgress().stream()
            .map(value -> value.participantId() == participantId
                ? new Progress(
                    participantId,
                    value.acceptedBoundary(),
                    Long.compareUnsigned(value.replayCursor(), sequence) >= 0
                        ? value.replayCursor()
                        : sequence)
                : value)
            .toList();
        Progress selected = progress.stream()
            .filter(value -> value.participantId() == participantId)
            .findFirst()
            .orElseThrow(() -> invalid("replay participant"));
        if (Long.compareUnsigned(sequence, selected.acceptedBoundary()) > 0) {
            throw invalid("replay sequence");
        }
        List<Completion> completions = new ArrayList<>(
            envelope.terminalCompletions());
        if (completion != null && completions.stream().noneMatch(value ->
                sameCompletion(value, completion))) {
            completions.add(completion);
        }
        completions.sort((left, right) -> {
            int participant = Long.compareUnsigned(
                left.participantId(), right.participantId());
            return participant != 0
                ? participant
                : Long.compareUnsigned(left.sequence(), right.sequence());
        });
        return decode(encodeSuccessor(envelope, progress, completions));
    }

    public static Envelope completeDelivery(
        Envelope envelope,
        long operationHigh,
        long operationLow,
        String sourceOwnerId,
        long sourceOwnerLeaseGeneration,
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        int deliveryState) {
        if (deliveryState < 1 || deliveryState > 3) {
            throw invalid("terminal delivery state");
        }
        boolean[] found = {false};
        List<Completion> completions = envelope.terminalCompletions().stream()
            .map(value -> {
                if (value.operationHigh() == operationHigh
                    && value.operationLow() == operationLow
                    && value.sourceOwnerId().equals(sourceOwnerId)
                    && value.sourceOwnerLeaseGeneration()
                        == sourceOwnerLeaseGeneration
                    && value.sourceNodeRid().equals(sourceNodeRid.toString())
                    && value.sourceNodeGeneration() == sourceNodeGeneration) {
                    found[0] = true;
                    return new Completion(
                        value.operationHigh(), value.operationLow(),
                        value.sourceOwnerId(),
                        value.sourceOwnerLeaseGeneration(),
                        value.sourceNodeRid(), value.sourceNodeGeneration(),
                        value.participantId(), value.sequence(),
                        value.terminalResult(), value.failureCode(),
                        deliveryState, value.payload());
                }
                return value;
            })
            .toList();
        if (!found[0]) {
            throw invalid("terminal completion identity");
        }
        return decode(encodeSuccessor(
            envelope, envelope.participantProgress(), completions));
    }

    public static Envelope putTerminalCompletion(
        Envelope envelope,
        Completion completion) {
        Objects.requireNonNull(envelope, "envelope");
        Objects.requireNonNull(completion, "completion");
        List<Completion> completions = new ArrayList<>(
            envelope.terminalCompletions());
        int existing = -1;
        for (int index = 0; index < completions.size(); index++) {
            if (sameCompletion(completions.get(index), completion)) {
                existing = index;
                break;
            }
        }
        if (existing >= 0) {
            Completion current = completions.get(existing);
            if (current.participantId() != completion.participantId()
                || current.sequence() != completion.sequence()
                || current.terminalResult() != completion.terminalResult()
                || current.failureCode() != completion.failureCode()
                || current.payload() == null != (completion.payload() == null)
                || current.payload() != null
                    && (!current.payload().packetName().equals(
                            completion.payload().packetName())
                        || !current.payload().contentType().equals(
                            completion.payload().contentType())
                        || !Arrays.equals(
                            current.payload().bytes(),
                            completion.payload().bytes()))) {
                throw invalid("terminal completion identity conflict");
            }
            if (completion.deliveryState() < current.deliveryState()
                || completion.deliveryState() > current.deliveryState() + 1) {
                throw invalid("terminal completion delivery transition");
            }
            completions.set(existing, completion);
        } else {
            completions.add(completion);
        }
        completions.sort((left, right) -> {
            int participant = Long.compareUnsigned(
                left.participantId(), right.participantId());
            return participant != 0
                ? participant
                : Long.compareUnsigned(left.sequence(), right.sequence());
        });
        return decode(encodeSuccessor(
            envelope, envelope.participantProgress(), completions));
    }

    private static boolean sameCompletion(
        Completion left, Completion right) {
        return left.operationHigh() == right.operationHigh()
            && left.operationLow() == right.operationLow()
            && left.sourceOwnerId().equals(right.sourceOwnerId())
            && left.sourceOwnerLeaseGeneration()
                == right.sourceOwnerLeaseGeneration()
            && left.sourceNodeRid().equals(right.sourceNodeRid())
            && left.sourceNodeGeneration() == right.sourceNodeGeneration();
    }

    private static void writeApplicationPayload(Writer writer, Payload value) {
        Writer body = new Writer();
        body.text8(value.packetName());
        body.text8(value.contentType());
        body.bytes32(value.bytes());
        writer.u8(1);
        writer.u32(body.size());
        writer.bytes(body.toByteArray());
    }

    private static List<JournalEntry> readJournal(
        Reader reader,
        Map<Long, Progress> progress) {
        int count = reader.count(MAX_RECORDS, "journal");
        if (count > reader.remaining()) {
            throw invalid("journal count exceeds remaining bytes");
        }
        List<JournalEntry> entries = new ArrayList<>(count);
        long previousParticipant = 0;
        long previousSequence = 0;
        for (int index = 0; index < count; index++) {
            int entryStart = reader.position();
            long participantId = reader.nonzeroU64("journal participant id");
            long sequence = reader.nonzeroU64("journal sequence");
            Progress participant = progress.get(participantId);
            if (participant == null
                || Long.compareUnsigned(participantId, previousParticipant) < 0
                || participantId == previousParticipant
                    && Long.compareUnsigned(sequence, previousSequence) <= 0
                || Long.compareUnsigned(sequence, participant.replayCursor()) <= 0
                || Long.compareUnsigned(sequence, participant.acceptedBoundary()) > 0) {
                throw invalid("journal order");
            }
            previousParticipant = participantId;
            previousSequence = sequence;
            readFrozenRecord(reader);
            entries.add(new JournalEntry(
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

    private static List<Completion> readTerminalCompletions(
        Reader reader,
        Set<Long> participants) {
        int count = reader.count(MAX_RECORDS, "terminal completion");
        if (count > reader.remaining()) {
            throw invalid(
                "terminal completion count exceeds remaining bytes");
        }
        List<Completion> values = new ArrayList<>(count);
        Set<OperationKey> operations = new HashSet<>();
        long previousParticipant = 0;
        long previousSequence = 0;
        for (int index = 0; index < count; index++) {
            long operationHigh = reader.u64();
            long operationLow = reader.u64();
            String sourceOwnerId = reader.text8();
            long sourceOwnerLease = reader.nonzeroU64("request source lease");
            String sourceNodeRid = reader.text8();
            long sourceNodeGeneration = reader.nonzeroU64(
                "request source node generation");
            long participantId = reader.nonzeroU64(
                "completion participant id");
            long sequence = reader.nonzeroU64("completion sequence");
            int result = terminalResult(reader.u32());
            int failureCode = reader.u32();
            int deliveryState = reader.u8();
            if (deliveryState > 3) {
                throw invalid("completion delivery state");
            }
            Payload payload = reader.bool() ? readApplicationPayload(reader) : null;
            OperationKey key = new OperationKey(
                sourceOwnerId,
                sourceOwnerLease,
                sourceNodeRid,
                sourceNodeGeneration,
                operationHigh,
                operationLow);
            if (!participants.contains(participantId)
                || Long.compareUnsigned(participantId, previousParticipant) < 0
                || participantId == previousParticipant
                    && Long.compareUnsigned(sequence, previousSequence) <= 0
                || !operations.add(key)) {
                throw invalid("terminal completion order");
            }
            previousParticipant = participantId;
            previousSequence = sequence;
            values.add(new Completion(
                operationHigh,
                operationLow,
                sourceOwnerId,
                sourceOwnerLease,
                sourceNodeRid,
                sourceNodeGeneration,
                participantId,
                sequence,
                result,
                failureCode,
                deliveryState,
                payload));
        }
        return values;
    }

    private static Payload readApplicationPayload(Reader reader) {
        if (reader.u8() != 1) {
            throw invalid("application payload version");
        }
        Reader body = reader.body32();
        Payload value = new Payload(
            body.text8(),
            body.text8(),
            body.bytes32());
        body.end("application payload");
        return value;
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

    public record Progress(long participantId, long acceptedBoundary, long replayCursor) {
        public Progress {
            if (participantId == 0
                || Long.compareUnsigned(replayCursor, acceptedBoundary) > 0) {
                throw invalid("participant progress");
            }
        }
    }

    public record Payload(String packetName, String contentType, byte[] bytes) {
        public Payload {
            Objects.requireNonNull(packetName, "packetName");
            Objects.requireNonNull(contentType, "contentType");
            bytes = Objects.requireNonNull(bytes, "bytes").clone();
        }

        @Override public byte[] bytes() { return bytes.clone(); }
    }

    public record Completion(
        long operationHigh,
        long operationLow,
        String sourceOwnerId,
        long sourceOwnerLeaseGeneration,
        String sourceNodeRid,
        long sourceNodeGeneration,
        long participantId,
        long sequence,
        int terminalResult,
        int failureCode,
        int deliveryState,
        Payload payload) {
        public Completion {
            Objects.requireNonNull(sourceOwnerId, "sourceOwnerId");
            Objects.requireNonNull(sourceNodeRid, "sourceNodeRid");
            if (sourceOwnerLeaseGeneration == 0 || sourceNodeGeneration == 0
                || participantId == 0 || sequence == 0
                || deliveryState < 0 || deliveryState > 3) {
                throw invalid("terminal completion");
            }
            ZLinkServiceRelocationEnvelopeCodec.terminalResult(
                terminalResult);
        }
    }

    public record Envelope(
        long relocationHigh,
        long relocationLow,
        ObjectIdentity object,
        long applicationVersion,
        List<ApplicationState> applicationStates,
        List<Progress> participantProgress,
        List<Completion> terminalCompletions,
        List<TimerRegistration> timerRegistrations,
        List<PendingTimerTick> pendingTimerTicks,
        byte[] canonicalBytes,
        byte[] canonicalPrefix,
        List<JournalEntry> journal,
        byte[] canonicalAfterJournal) {
        public Envelope {
            participantProgress = List.copyOf(participantProgress);
            applicationStates = List.copyOf(applicationStates);
            terminalCompletions = List.copyOf(terminalCompletions);
            timerRegistrations = List.copyOf(timerRegistrations);
            pendingTimerTicks = List.copyOf(pendingTimerTicks);
            canonicalBytes = canonicalBytes.clone();
            canonicalPrefix = canonicalPrefix.clone();
            journal = List.copyOf(journal);
            canonicalAfterJournal = canonicalAfterJournal.clone();
        }

        @Override public byte[] canonicalBytes() { return canonicalBytes.clone(); }
        @Override public byte[] canonicalPrefix() { return canonicalPrefix.clone(); }
        @Override public byte[] canonicalAfterJournal() {
            return canonicalAfterJournal.clone();
        }

        public int pendingRelayCount() {
            return (int) terminalCompletions.stream()
                .filter(value -> value.deliveryState() == 0)
                .count();
        }

        public boolean recoveryReleaseEligible() {
            return terminalCompletions.stream().allMatch(value ->
                value.deliveryState() == 2 || value.deliveryState() == 3);
        }
    }

    public record JournalEntry(
        long participantId,
        long sequence,
        byte[] rawEntry) {
        public JournalEntry {
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
                    "journal entry does not contain a frozen operation");
            }
            return java.util.Arrays.copyOfRange(
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

    private record OperationKey(
        String sourceOwnerId,
        long sourceOwnerLeaseGeneration,
        String sourceNodeRid,
        long sourceNodeGeneration,
        long operationHigh,
        long operationLow) {
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

    private static final class Writer {
        private final ByteArrayOutputStream output = new ByteArrayOutputStream();
        int size() { return output.size(); }
        void u8(int value) {
            if (value < 0 || value > 0xff) throw invalid("u8");
            output.write(value);
        }
        void u32(int value) {
            output.writeBytes(ByteBuffer.allocate(4).order(ByteOrder.BIG_ENDIAN)
                .putInt(value).array());
        }
        void u64(long value) {
            output.writeBytes(ByteBuffer.allocate(8).order(ByteOrder.BIG_ENDIAN)
                .putLong(value).array());
        }
        void text8(String value) {
            byte[] encoded = Objects.requireNonNull(value, "value")
                .getBytes(StandardCharsets.UTF_8);
            if (encoded.length < 1 || encoded.length > 255) {
                throw invalid("text8");
            }
            u8(encoded.length);
            bytes(encoded);
        }
        void bytes32(byte[] value) {
            u32(value.length);
            bytes(value);
        }
        void bytes(byte[] value) { output.writeBytes(value); }
        byte[] toByteArray() { return output.toByteArray(); }
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

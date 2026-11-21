package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.contracts.core.RoutingId;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Objects;
import java.util.UUID;
import java.util.zip.CRC32C;

/** Compatibility reader for the previous authority-payload-v1 relocation slot. */
public final class ZLinkCanonicalRelocationAuthorityStateCodec {
    private static final byte[] MAGIC = {0x5a, 0x4c, 0x41, 0x55};
    private static final byte[] EMPTY = {0, 0, 0, 0, 0};
    private static final int MAX_AUTHORITY_ENVELOPE_BYTES = 1_048_576;
    private static final long MAX_ISSUED_AGGREGATE_GENERATION = Long.MAX_VALUE - 1;

    private ZLinkCanonicalRelocationAuthorityStateCodec() {}

    // Strips the relocation slot by its envelope boundaries only. Application
    // payload extraction does not need to interpret relocation progress.
    public static byte[] applicationPayloadOrOriginal(byte[] payload) {
        Objects.requireNonNull(payload, "payload");
        try {
            Slot slot = slot(payload);
            return slot.present() ? clearSlot(slot) : payload.clone();
        } catch (RuntimeException invalid) {
            return payload.clone();
        }
    }

    static Published decode(byte[] authorityPayload) {
        try {
            return decodeStrict(authorityPayload);
        } catch (RuntimeException invalid) {
            return null;
        }
    }

    private static Published decodeStrict(byte[] authorityPayload) {
        Slot slot = slot(authorityPayload);
        if (!slot.present()) {
            return null;
        }
        State current = decodeState(slot.state(), null);
        return new Published(
                new UUID(current.relocationHigh(), current.relocationLow()),
                current.aggregateGeneration(),
                current.targetAttemptGeneration(),
                current.relocationReference(),
                current.relocationChecksumCrc32c(),
                current.sourceOwnerId(),
                current.sourceOwnerLeaseGeneration(),
                current.sourceNodeRid(),
                current.sourceNodeGeneration(),
                current.targetNodeRid(),
                current.targetNodeGeneration(),
                current.targetOwnerId(),
                current.targetOwnerLeaseGeneration(),
                current.coordinatorOwnerId(),
                current.coordinatorLeaseGeneration(),
                current.coordinatorNodeRid(),
                current.coordinatorNodeGeneration(),
                current.coordinatorExpectedStoreVersion(),
                current.phase(),
                current.applicationVersion(),
                current.sourceCleanupState(),
                clearSlot(slot));
    }

    static State decodeState(byte[] encoded, Long expectedRootAggregateGeneration) {
        Reader state = new Reader(encoded);
        if (state.u8() != 1) {
            throw invalid();
        }
        Reader body = state.reader(state.u32());
        if (!state.end()) {
            throw invalid();
        }
        long relocationHigh = body.u64();
        long relocationLow = body.u64();
        long aggregateGeneration = body.ordinalOrZero();
        long targetAttemptGeneration = body.ordinalOrZero();
        String relocationReference = body.text16();
        long relocationChecksumCrc32c = body.u32Unsigned();
        RoutingId sourceNodeRid = RoutingId.from(body.sized8());
        long sourceNodeGeneration = body.nonzeroU64();
        String sourceOwnerId = body.text8();
        long sourceOwnerLeaseGeneration = body.nonzeroU64();
        RoutingId targetNodeRid = RoutingId.from(body.optionalSized8());
        long targetNodeGeneration = body.ordinalOrZero();
        String targetOwnerId = body.optionalText8();
        long targetOwnerLeaseGeneration = body.ordinalOrZero();
        String coordinatorOwnerId = body.text8();
        long coordinatorLeaseGeneration = body.nonzeroU64();
        RoutingId coordinatorNodeRid = RoutingId.from(body.sized8());
        long coordinatorNodeGeneration = body.nonzeroU64();
        String coordinatorExpectedStoreVersion = body.optionalText8();
        int phase = body.u8();
        long applicationVersion = body.u64();
        int sourceCleanupState = body.u8();
        if (!body.end()) {
            throw invalid();
        }
        State value =
                new State(
                        relocationHigh,
                        relocationLow,
                        aggregateGeneration,
                        targetAttemptGeneration,
                        relocationReference,
                        relocationChecksumCrc32c,
                        sourceNodeRid,
                        sourceNodeGeneration,
                        sourceOwnerId,
                        sourceOwnerLeaseGeneration,
                        targetNodeRid,
                        targetNodeGeneration,
                        targetOwnerId,
                        targetOwnerLeaseGeneration,
                        coordinatorOwnerId,
                        coordinatorLeaseGeneration,
                        coordinatorNodeRid,
                        coordinatorNodeGeneration,
                        coordinatorExpectedStoreVersion,
                        phase,
                        applicationVersion,
                        sourceCleanupState);
        validateState(value, expectedRootAggregateGeneration);
        return value;
    }

    private static void validateState(State value, Long expectedRootAggregateGeneration) {
        boolean emptyTarget =
                value.targetNodeRid().toBytes().length == 0
                        && value.targetNodeGeneration() == 0
                        && value.targetOwnerId().isEmpty()
                        && value.targetOwnerLeaseGeneration() == 0;
        boolean completeTarget =
                value.targetNodeRid().toBytes().length != 0
                        && value.targetNodeGeneration() != 0
                        && !value.targetOwnerId().isEmpty()
                        && value.targetOwnerLeaseGeneration() != 0;
        if (value.relocationHigh() == 0 && value.relocationLow() == 0
                || value.aggregateGeneration() < 0
                || value.aggregateGeneration() > MAX_ISSUED_AGGREGATE_GENERATION
                || value.targetAttemptGeneration() < 0
                || value.relocationReference().isEmpty()
                || value.relocationChecksumCrc32c() < 0
                || value.relocationChecksumCrc32c() > 0xffff_ffffL
                || value.sourceNodeRid().toBytes().length == 0
                || value.sourceNodeGeneration() == 0
                || value.sourceOwnerId().isEmpty()
                || value.sourceOwnerLeaseGeneration() == 0
                || value.coordinatorOwnerId().isEmpty()
                || value.coordinatorLeaseGeneration() == 0
                || value.coordinatorNodeRid().toBytes().length == 0
                || value.coordinatorNodeGeneration() == 0
                || value.phase() < 1
                || value.phase() > 9
                || value.applicationVersion() < 0
                || value.sourceCleanupState() < 0
                || value.sourceCleanupState() > 2
                || value.phase() == 1 && value.aggregateGeneration() != 0
                || value.phase() != 1 && value.aggregateGeneration() == 0
                || (value.phase() == 1 || value.phase() == 2)
                        && (!emptyTarget || value.targetAttemptGeneration() != 0)
                || value.phase() >= 3
                        && value.phase() <= 8
                        && (!completeTarget || value.targetAttemptGeneration() == 0)
                || expectedRootAggregateGeneration != null
                        && (value.phase() == 1
                                || value.aggregateGeneration()
                                        != expectedRootAggregateGeneration.longValue())) {
            throw invalid();
        }
    }

    private static byte[] clearSlot(Slot slot) {
        byte[] state = EMPTY;
        byte[] nextBody = new byte[slot.start() + state.length + slot.body().length - slot.end()];
        System.arraycopy(slot.body(), 0, nextBody, 0, slot.start());
        System.arraycopy(state, 0, nextBody, slot.start(), state.length);
        System.arraycopy(
                slot.body(),
                slot.end(),
                nextBody,
                slot.start() + state.length,
                slot.body().length - slot.end());
        return envelope(nextBody);
    }

    private static byte[] envelope(byte[] body) {
        if (body.length + 15 > MAX_AUTHORITY_ENVELOPE_BYTES) throw invalid();
        ByteBuffer bytes = ByteBuffer.allocate(body.length + 15).order(ByteOrder.BIG_ENDIAN);
        bytes.put(MAGIC).put((byte) 1).putShort((short) 0).putInt(body.length).put(body);
        bytes.putInt((int) crc32c(bytes.array(), bytes.position()));
        return bytes.array();
    }

    private static Slot slot(byte[] payload) {
        Objects.requireNonNull(payload, "payload");
        if (payload.length > MAX_AUTHORITY_ENVELOPE_BYTES) {
            throw invalid();
        }
        Reader envelope = new Reader(payload);
        envelope.expect(MAGIC);
        if (envelope.u8() != 1 || envelope.u16() != 0) throw invalid();
        Reader body = envelope.reader(envelope.u32());
        int checksumOffset = envelope.offset();
        if (envelope.u32Unsigned() != crc32c(payload, checksumOffset) || !envelope.end())
            throw invalid();
        AuthorityShape shape = authorityPrefix(body);
        int start = body.offset();
        int present = body.u8();
        long encodedSize = body.u32Unsigned();
        if (present != 0 && present != 1
                || present == 0 && encodedSize != 0
                || present == 1 && encodedSize == 0) {
            throw invalid();
        }
        int size = Math.toIntExact(encodedSize);
        body.skip(size);
        int end = body.offset();
        validateActivationRecovery(body, shape);
        if (!body.end()) throw invalid();
        return new Slot(
                body.bytes(),
                start,
                end,
                Arrays.copyOfRange(body.bytes(), start, end),
                present == 1);
    }

    private static AuthorityShape authorityPrefix(Reader body) {
        int operationKind = body.u8();
        int objectKind = body.u8();
        Reader object = body.reader(body.u16());
        int objectState;
        int authoritySpotKind = 0;
        if (objectKind == 1) {
            object.text8();
            object.text8();
            objectState = object.u8();
            object.text8();
            object.nonzeroU64();
            int currentSpotKind = object.u8();
            if (objectState < 0
                    || objectState > 1
                    || (currentSpotKind != 1 && currentSpotKind != 2)
                    || operationKind != (objectState == 0 ? 1 : 0)) {
                throw invalid();
            }
        } else if (objectKind == 2) {
            int spotKind = object.u8();
            authoritySpotKind = spotKind;
            Reader spot = object.reader(object.u16());
            if (spotKind == 2) {
                spot.text8();
                spot.text8();
                objectState = spot.u8();
                if (objectState < 0
                        || objectState > 2
                        || operationKind
                                != switch (objectState) {
                                    case 0 -> 1;
                                    case 1 -> 0;
                                    case 2 -> 3;
                                    default -> -1;
                                }) {
                    throw invalid();
                }
            } else if (spotKind == 3) {
                objectState = spot.u8();
                Reader instance = spot.reader(spot.u16());
                instance.text8();
                instance.text8();
                if (!instance.end()
                        || objectState < 1
                        || objectState > 3
                        || operationKind
                                != switch (objectState) {
                                    case 1 -> 1;
                                    case 2 -> 0;
                                    case 3 -> 3;
                                    default -> -1;
                                }) {
                    throw invalid();
                }
            } else {
                throw invalid();
            }
            if (!spot.end()) throw invalid();
        } else {
            throw invalid();
        }
        if (!object.end()) throw invalid();
        body.text8();
        body.nonzeroU64();
        body.text8();
        RoutingId.from(body.sized8());
        body.nonzeroU64();
        return new AuthorityShape(operationKind, objectKind, objectState, authoritySpotKind);
    }

    private static void validateActivationRecovery(Reader body, AuthorityShape shape) {
        int present = body.u8();
        long encodedSize = body.u32Unsigned();
        if (present == 0) {
            if (encodedSize != 0) throw invalid();
            return;
        }
        if (present != 1
                || shape.objectKind() != 2
                || shape.spotKind() != 3
                || shape.objectState() != 1
                || shape.operationKind() != 0) {
            throw invalid();
        }
        Reader recovery = body.reader(Math.toIntExact(encodedSize));
        recovery.text16();
        if (recovery.u8() != 32) throw invalid();
        recovery.skip(32);
        long requestSize = recovery.u32Unsigned();
        long inboxSequence = recovery.nonzeroU64();
        long replayCursor = recovery.ordinalOrZero();
        if (requestSize > MAX_AUTHORITY_ENVELOPE_BYTES
                || Long.compareUnsigned(replayCursor, inboxSequence) > 0
                || !recovery.end()) {
            throw invalid();
        }
    }

    record Published(
            UUID aggregateId,
            long aggregateGeneration,
            long targetAttemptGeneration,
            String relocationReference,
            long relocationChecksumCrc32c,
            String sourceOwnerId,
            long sourceOwnerLeaseGeneration,
            RoutingId sourceNodeRid,
            long sourceNodeGeneration,
            RoutingId targetNodeRid,
            long targetNodeGeneration,
            String targetOwnerId,
            long targetOwnerLeaseGeneration,
            String coordinatorOwnerId,
            long coordinatorLeaseGeneration,
            RoutingId coordinatorNodeRid,
            long coordinatorNodeGeneration,
            String coordinatorExpectedStoreVersion,
            int phase,
            long applicationVersion,
            int sourceCleanupState,
            byte[] applicationPayload) {
        Published {
            applicationPayload = applicationPayload.clone();
        }

        @Override
        public byte[] applicationPayload() {
            return applicationPayload.clone();
        }
    }

    record State(
            long relocationHigh,
            long relocationLow,
            long aggregateGeneration,
            long targetAttemptGeneration,
            String relocationReference,
            long relocationChecksumCrc32c,
            RoutingId sourceNodeRid,
            long sourceNodeGeneration,
            String sourceOwnerId,
            long sourceOwnerLeaseGeneration,
            RoutingId targetNodeRid,
            long targetNodeGeneration,
            String targetOwnerId,
            long targetOwnerLeaseGeneration,
            String coordinatorOwnerId,
            long coordinatorLeaseGeneration,
            RoutingId coordinatorNodeRid,
            long coordinatorNodeGeneration,
            String coordinatorExpectedStoreVersion,
            int phase,
            long applicationVersion,
            int sourceCleanupState) {
        State {
            Objects.requireNonNull(relocationReference, "relocationReference");
            Objects.requireNonNull(sourceNodeRid, "sourceNodeRid");
            Objects.requireNonNull(sourceOwnerId, "sourceOwnerId");
            Objects.requireNonNull(targetNodeRid, "targetNodeRid");
            Objects.requireNonNull(targetOwnerId, "targetOwnerId");
            Objects.requireNonNull(coordinatorOwnerId, "coordinatorOwnerId");
            Objects.requireNonNull(coordinatorNodeRid, "coordinatorNodeRid");
            Objects.requireNonNull(
                    coordinatorExpectedStoreVersion, "coordinatorExpectedStoreVersion");
        }
    }

    private record AuthorityShape(
            int operationKind, int objectKind, int objectState, int spotKind) {}

    private record Slot(byte[] body, int start, int end, byte[] state, boolean present) {}

    private static long crc32c(byte[] bytes) {
        return crc32c(bytes, bytes.length);
    }

    private static long crc32c(byte[] bytes, int length) {
        CRC32C crc = new CRC32C();
        crc.update(bytes, 0, length);
        return crc.getValue();
    }

    private static IllegalArgumentException invalid() {
        return new IllegalArgumentException("invalid canonical authority payload");
    }

    private static final class Reader {
        private final byte[] bytes;
        private int offset;

        Reader(byte[] bytes) {
            this.bytes = Objects.requireNonNull(bytes);
        }

        void expect(byte[] v) {
            for (byte b : v) if (u8() != Byte.toUnsignedInt(b)) throw invalid();
        }

        int u8() {
            require(1);
            return Byte.toUnsignedInt(bytes[offset++]);
        }

        int u16() {
            require(2);
            int v = (u8() << 8) | u8();
            return v;
        }

        int u32() {
            long v = u32Unsigned();
            if (v > Integer.MAX_VALUE) throw invalid();
            return (int) v;
        }

        long u32Unsigned() {
            require(4);
            long v =
                    Integer.toUnsignedLong(
                            ByteBuffer.wrap(bytes, offset, 4).order(ByteOrder.BIG_ENDIAN).getInt());
            offset += 4;
            return v;
        }

        long u64() {
            require(8);
            long v = ByteBuffer.wrap(bytes, offset, 8).order(ByteOrder.BIG_ENDIAN).getLong();
            offset += 8;
            return v;
        }

        long nonzeroU64() {
            long v = u64();
            if (v == 0) throw invalid();
            return v;
        }

        long ordinalOrZero() {
            long v = u64();
            if (v < 0) throw invalid();
            return v;
        }

        String text8() {
            return text(sized8());
        }

        String text16() {
            int n = u16();
            if (n < 1 || n > 4096) throw invalid();
            return text(take(n));
        }

        byte[] sized8() {
            int n = u8();
            if (n == 0) throw invalid();
            return take(n);
        }

        // optional-text8 / optional-rid: zero length means absent.
        String optionalText8() {
            int n = u8();
            return n == 0 ? "" : text(take(n));
        }

        byte[] optionalSized8() {
            int n = u8();
            return n == 0 ? new byte[0] : take(n);
        }

        Reader reader(int n) {
            return new Reader(take(n));
        }

        void skip(int n) {
            take(n);
        }

        byte[] take(int n) {
            require(n);
            byte[] v = Arrays.copyOfRange(bytes, offset, offset + n);
            offset += n;
            return v;
        }

        int offset() {
            return offset;
        }

        int remaining() {
            return bytes.length - offset;
        }

        byte[] bytes() {
            return bytes;
        }

        boolean end() {
            return offset == bytes.length;
        }

        void require(int n) {
            if (n < 0 || offset + n > bytes.length) throw invalid();
        }

        private static String text(byte[] value) {
            try {
                String decoded =
                        StandardCharsets.UTF_8
                                .newDecoder()
                                .onMalformedInput(CodingErrorAction.REPORT)
                                .onUnmappableCharacter(CodingErrorAction.REPORT)
                                .decode(ByteBuffer.wrap(value))
                                .toString();
                if (decoded.indexOf('\0') >= 0) throw invalid();
                return decoded;
            } catch (CharacterCodingException invalidText) {
                throw invalid();
            }
        }
    }
}

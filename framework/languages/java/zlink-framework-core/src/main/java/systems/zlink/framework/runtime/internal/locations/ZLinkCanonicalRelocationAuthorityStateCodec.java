package systems.zlink.framework.runtime.internal.locations;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Objects;
import java.util.UUID;
import java.util.zip.CRC32C;
import systems.zlink.contracts.core.RoutingId;

/** Reads and replaces the canonical relocation slot in authority-payload-v1. */
public final class ZLinkCanonicalRelocationAuthorityStateCodec {
    private static final byte[] MAGIC = {0x5a, 0x4c, 0x41, 0x55};
    private static final byte[] EMPTY = {0, 0, 0, 0, 0};
    private static final int MAX_AUTHORITY_ENVELOPE_BYTES = 1_048_576;

    private ZLinkCanonicalRelocationAuthorityStateCodec() {
    }

    public static byte[] applicationPayloadOrOriginal(byte[] payload) {
        Published publication = decode(payload);
        return publication == null
            ? Objects.requireNonNull(payload, "payload").clone()
            : publication.applicationPayload();
    }

    static byte[] publish(
        byte[] authorityPayload,
        ZLinkAggregateRelocationCoordinator.Request request,
        ZLinkAuthorityGenerationTransition ownerTransition,
        ZLinkRelocationStored stored,
        boolean sourceCleanupCompleted) {
        Objects.requireNonNull(request, "request");
        Objects.requireNonNull(ownerTransition, "ownerTransition");
        Objects.requireNonNull(stored, "stored");
        Published previous = decodeStrict(authorityPayload);
        byte[] applicationPayload = previous == null
            ? Objects.requireNonNull(authorityPayload, "authorityPayload").clone()
            : previous.applicationPayload();
        Owner source = previous == null
            ? owner(applicationPayload)
            : new Owner(
                previous.sourceOwnerId(),
                previous.sourceOwnerLeaseGeneration(),
                null,
                previous.sourceNodeRid(),
                previous.sourceNodeGeneration());
        var root = ZLinkServiceRelocationEnvelopeCodec.decode(request.root());
        if (root.relocationHigh() != request.aggregateId().getMostSignificantBits()
            || root.relocationLow() != request.aggregateId().getLeastSignificantBits()) {
            throw new IllegalArgumentException(
                "authority relocation identity differs from canonical root");
        }
        if (previous != null) {
            validateSuccessor(previous, request);
        }
        applicationPayload = projectOwner(
            applicationPayload, request, ownerTransition);
        Writer body = new Writer();
        body.u64(root.relocationHigh());
        body.u64(root.relocationLow());
        body.u64(request.aggregateGeneration());
        body.sized8(source.nodeRid().toBytes());
        body.u64(source.nodeGeneration());
        body.text8(source.ownerId());
        body.u64(source.ownerLeaseGeneration());
        body.sized8(request.targetDescriptor().rid().toBytes());
        body.u64(request.targetDescriptorLifecycleGeneration());
        body.text8(request.targetOwner().ownerId());
        body.u64(request.targetOwner().leaseGeneration());
        body.u64(0);
        body.text8(request.targetOwner().ownerId());
        body.u64(request.targetOwner().leaseGeneration());
        body.sized8(request.targetDescriptor().rid().toBytes());
        body.u64(request.targetDescriptorLifecycleGeneration());
        body.u8(sourceCleanupCompleted ? 8 : 4);
        Writer pointer = new Writer();
        pointer.text16(stored.reference());
        pointer.u32(stored.checksumCrc32c());
        body.u8(1);
        body.u16(pointer.size());
        body.raw(pointer.bytes());
        body.u64(root.applicationVersion());
        body.u32(root.participantProgress().size());
        for (var progress : root.participantProgress()) {
            body.u64(progress.participantId());
            body.u64(progress.acceptedBoundary());
            body.u64(progress.replayCursor());
        }
        body.u32(root.terminalCompletions().size());
        body.u32(root.terminalCompletions().stream()
            .filter(value -> value.deliveryState() == 0).count());
        body.u8(sourceCleanupCompleted ? 1 : 0);
        Writer state = new Writer();
        state.u8(1);
        state.u32(body.size());
        state.raw(body.bytes());
        return replace(
            replaceOwner(applicationPayload, request),
            state.bytes());
    }

    static Published decode(byte[] authorityPayload) {
        try {
            return decodeStrict(authorityPayload);
        } catch (RuntimeException invalid) {
            return null;
        }
    }

    /** Returns the committed marker's initial root projection. */
    public static ZLinkAggregateProgress progress(byte[] authorityPayload) {
        Published publication = decodeStrict(authorityPayload);
        return new ZLinkAggregateProgress(
            publication.reference(),
            publication.checksumCrc32c(),
            publication.sourceCleanupCompleted() ? 8 : 4,
            publication.sourceCleanupCompleted(),
            publication.terminalCompletionCount(),
            publication.pendingRelayCount());
    }

    private static Published decodeStrict(byte[] authorityPayload) {
        Slot slot = slot(authorityPayload);
        if (!slot.present()) {
            return null;
        }
        Current current = current(slot);
        return new Published(
            current.reference(), current.checksumCrc32c(),
            new UUID(current.relocationHigh(), current.relocationLow()),
            current.aggregateGeneration(),
            current.sourceOwnerId(),
            current.sourceOwnerLeaseGeneration(),
            current.sourceNodeRid(),
            current.sourceNodeGeneration(),
            current.targetNodeRid(),
            current.targetNodeGeneration(),
            current.targetOwnerId(), current.targetOwnerLeaseGeneration(),
            replace(slot, EMPTY),
            current.sourceCleanupCompleted(),
            current.terminalCompletionCount(),
            current.pendingRelayCount());
    }

    static byte[] replaceRoot(
        byte[] authorityPayload,
        ZLinkRelocationStored stored,
        ZLinkServiceRelocationEnvelopeCodec.Envelope root) {
        return replaceRoot(authorityPayload, stored, root, false);
    }

    static byte[] completeSourceCleanup(
        byte[] authorityPayload,
        ZLinkRelocationStored stored,
        ZLinkServiceRelocationEnvelopeCodec.Envelope root) {
        return replaceRoot(authorityPayload, stored, root, true);
    }

    private static byte[] replaceRoot(
        byte[] authorityPayload,
        ZLinkRelocationStored stored,
        ZLinkServiceRelocationEnvelopeCodec.Envelope root,
        boolean completeSourceCleanup) {
        Current current = current(authorityPayload);
        if (root.relocationHigh() != current.relocationHigh()
            || root.relocationLow() != current.relocationLow()) {
            throw new IllegalArgumentException(
                "successor root has a different relocation identity");
        }
        Writer body = new Writer();
        body.u64(current.relocationHigh());
        body.u64(current.relocationLow());
        body.u64(current.aggregateGeneration());
        body.sized8(current.sourceNodeRid().toBytes());
        body.u64(current.sourceNodeGeneration());
        body.text8(current.sourceOwnerId());
        body.u64(current.sourceOwnerLeaseGeneration());
        body.sized8(current.targetNodeRid().toBytes());
        body.u64(current.targetNodeGeneration());
        body.text8(current.targetOwnerId());
        body.u64(current.targetOwnerLeaseGeneration());
        body.u64(current.placementReservationToken());
        body.text8(current.capacityOwnerId());
        body.u64(current.capacityOwnerLeaseGeneration());
        body.sized8(current.capacityDescriptorRid().toBytes());
        body.u64(current.capacityDescriptorGeneration());
        body.u8(completeSourceCleanup ? 8 : current.phase());
        Writer pointer = new Writer();
        pointer.text16(stored.reference());
        pointer.u32(stored.checksumCrc32c());
        body.u8(1);
        body.u16(pointer.size());
        body.raw(pointer.bytes());
        body.u64(root.applicationVersion());
        body.u32(root.participantProgress().size());
        for (var progress : root.participantProgress()) {
            body.u64(progress.participantId());
            body.u64(progress.acceptedBoundary());
            body.u64(progress.replayCursor());
        }
        body.u32(root.terminalCompletions().size());
        body.u32(root.terminalCompletions().stream()
            .filter(value -> value.deliveryState() == 0).count());
        body.u8(completeSourceCleanup
            || current.sourceCleanupCompleted() ? 1 : 0);
        Writer state = new Writer();
        state.u8(1);
        state.u32(body.size());
        state.raw(body.bytes());
        return replace(authorityPayload, state.bytes());
    }

    private static Current current(byte[] authorityPayload) {
        return current(slot(authorityPayload));
    }

    private static Current current(Slot slot) {
        if (!slot.present()) {
            throw invalid();
        }
        Reader state = new Reader(slot.state());
        if (state.u8() != 1) throw invalid();
        Reader body = state.reader(state.u32());
        if (!state.end()) throw invalid();
        long relocationHigh = body.u64();
        long relocationLow = body.u64();
        if (relocationHigh == 0 && relocationLow == 0) throw invalid();
        long aggregateGeneration = body.u64();
        if (aggregateGeneration <= 0) throw invalid();
        RoutingId sourceNodeRid = RoutingId.from(body.sized8());
        long sourceNodeGeneration = body.nonzeroU64();
        String sourceOwnerId = body.text8();
        long sourceOwnerLeaseGeneration = body.nonzeroU64();
        RoutingId targetNodeRid = RoutingId.from(body.sized8());
        long targetNodeGeneration = body.nonzeroU64();
        String targetOwnerId = body.text8();
        long targetOwnerLeaseGeneration = body.nonzeroU64();
        long placementReservationToken = body.u64();
        String capacityOwnerId = body.text8();
        long capacityOwnerLeaseGeneration = body.nonzeroU64();
        RoutingId capacityDescriptorRid = RoutingId.from(body.sized8());
        long capacityDescriptorGeneration = body.nonzeroU64();
        int phase = body.u8();
        if (phase != 4 && phase != 8 || body.u8() != 1) throw invalid();
        Reader pointer = body.reader(body.u16());
        String reference = pointer.text16();
        long checksumCrc32c = pointer.u32Unsigned();
        if (!pointer.end()) throw invalid();
        long applicationVersion = body.u64();
        if (applicationVersion < 0) throw invalid();
        int progress = body.u32();
        if (progress < 0 || progress > body.remaining() / 24) throw invalid();
        long previousParticipantId = 0;
        for (int index = 0; index < progress; index++) {
            long participantId = body.nonzeroU64();
            long acceptedBoundary = body.ordinalOrZero();
            long replayCursor = body.ordinalOrZero();
            if (Long.compareUnsigned(participantId, previousParticipantId) <= 0
                || Long.compareUnsigned(replayCursor, acceptedBoundary) > 0) {
                throw invalid();
            }
            previousParticipantId = participantId;
        }
        int terminalCompletionCount = body.u32();
        int pendingRelayCount = body.u32();
        if (pendingRelayCount > terminalCompletionCount) throw invalid();
        int cleanupState = body.u8();
        if (cleanupState != 0 && cleanupState != 1
            || cleanupState == 1 && phase != 8
            || cleanupState == 0 && phase == 8) {
            throw invalid();
        }
        boolean sourceCleanupCompleted = cleanupState == 1;
        if (!body.end()) throw invalid();
        return new Current(
            relocationHigh, relocationLow, aggregateGeneration,
            sourceNodeRid, sourceNodeGeneration,
            sourceOwnerId, sourceOwnerLeaseGeneration,
            targetNodeRid, targetNodeGeneration,
            targetOwnerId, targetOwnerLeaseGeneration,
            placementReservationToken,
            capacityOwnerId, capacityOwnerLeaseGeneration,
            capacityDescriptorRid, capacityDescriptorGeneration,
            phase, reference, checksumCrc32c, sourceCleanupCompleted,
            terminalCompletionCount, pendingRelayCount);
    }

    private static void validateSuccessor(
        Published previous,
        ZLinkAggregateRelocationCoordinator.Request request) {
        // Successor aggregate generations are derived from the canonical root
        // and cleanup state, so they are not an ordered counter. The stable
        // relocation identity and target owner fence are the continuity check.
        if (!previous.aggregateId().equals(request.aggregateId())
            || !previous.targetOwnerId().equals(
                request.targetOwner().ownerId())
            || previous.targetOwnerLeaseGeneration()
                != request.targetOwner().leaseGeneration()
            || !previous.targetNodeRid().equals(
                request.targetDescriptor().rid())
            || previous.targetNodeGeneration()
                != request.targetDescriptorLifecycleGeneration()) {
            throw new IllegalArgumentException(
                "canonical relocation successor fence differs");
        }
    }

    private static byte[] projectOwner(
        byte[] payload,
        ZLinkAggregateRelocationCoordinator.Request request,
        ZLinkAuthorityGenerationTransition ownerTransition) {
        if (ownerTransition == ZLinkAuthorityGenerationTransition.NEW_OWNER) {
            return replaceOwner(payload, request);
        }
        Owner current = owner(payload);
        if (!current.ownerId().equals(request.targetOwner().ownerId())
            || current.ownerLeaseGeneration()
                != request.targetOwner().leaseGeneration()
            || !current.meshName().equals(request.targetDescriptor().meshName())
            || !current.nodeRid().equals(request.targetDescriptor().rid())
            || current.nodeGeneration()
                != request.targetDescriptorLifecycleGeneration()) {
            throw new IllegalArgumentException(
                "preserved authority owner differs from target fence");
        }
        return payload.clone();
    }

    private static Owner owner(byte[] payload) {
        Slot slot = slot(payload);
        Reader body = new Reader(slot.body());
        body.u8(); body.u8(); body.skip(body.u16());
        String ownerId = body.text8();
        long lease = body.u64();
        String meshName = body.text8();
        RoutingId nodeRid = RoutingId.from(body.sized8());
        long nodeGeneration = body.u64();
        return new Owner(ownerId, lease, meshName, nodeRid, nodeGeneration);
    }

    private static byte[] replace(byte[] payload, byte[] state) {
        return replace(slot(payload), state);
    }

    private static byte[] replace(Slot slot, byte[] state) {
        byte[] nextBody = new byte[
            slot.start() + state.length + slot.body().length - slot.end()];
        System.arraycopy(slot.body(), 0, nextBody, 0, slot.start());
        System.arraycopy(state, 0, nextBody, slot.start(), state.length);
        System.arraycopy(slot.body(), slot.end(), nextBody,
            slot.start() + state.length, slot.body().length - slot.end());
        return envelope(nextBody);
    }

    /**
     * Projects the target Location Store owner into the application authority
     * envelope before the relocation slot is added. The relocation slot keeps
     * the original source owner separately for recovery and audit.
     */
    private static byte[] replaceOwner(
        byte[] payload,
        ZLinkAggregateRelocationCoordinator.Request request) {
        Slot slot = slot(payload);
        Reader body = new Reader(slot.body());
        body.u8();
        body.u8();
        body.skip(body.u16());
        int ownerStart = body.offset();
        body.text8();
        body.u64();
        body.text8();
        body.sized8();
        body.u64();
        int ownerEnd = body.offset();

        Writer owner = new Writer();
        owner.text8(request.targetOwner().ownerId());
        owner.u64(request.targetOwner().leaseGeneration());
        owner.text8(request.targetDescriptor().meshName());
        owner.sized8(request.targetDescriptor().rid().toBytes());
        owner.u64(request.targetDescriptorLifecycleGeneration());

        byte[] originalBody = slot.body();
        byte[] replacement = owner.bytes();
        byte[] nextBody = new byte[
            originalBody.length - (ownerEnd - ownerStart) + replacement.length];
        System.arraycopy(originalBody, 0, nextBody, 0, ownerStart);
        System.arraycopy(replacement, 0, nextBody, ownerStart,
            replacement.length);
        System.arraycopy(originalBody, ownerEnd, nextBody,
            ownerStart + replacement.length, originalBody.length - ownerEnd);
        return envelope(nextBody);
    }

    private static byte[] envelope(byte[] nextBody) {
        Writer envelope = new Writer();
        envelope.raw(MAGIC); envelope.u8(1); envelope.u16(0);
        envelope.u32(nextBody.length); envelope.raw(nextBody);
        byte[] withoutChecksum = envelope.bytes();
        envelope.u32(crc32c(withoutChecksum));
        byte[] encoded = envelope.bytes();
        if (encoded.length > MAX_AUTHORITY_ENVELOPE_BYTES) {
            throw invalid();
        }
        return encoded;
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
        if (envelope.u32Unsigned() != crc32c(payload, checksumOffset)
            || !envelope.end()) throw invalid();
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
        return new Slot(body.bytes(), start, end,
            Arrays.copyOfRange(body.bytes(), start, end), present == 1);
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
            if (objectState < 0 || objectState > 1
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
                if (objectState < 0 || objectState > 2
                    || operationKind != switch (objectState) {
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
                    || objectState < 1 || objectState > 3
                    || operationKind != switch (objectState) {
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
        return new AuthorityShape(
            operationKind, objectKind, objectState, authoritySpotKind);
    }

    private static void validateActivationRecovery(
        Reader body,
        AuthorityShape shape) {
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
        String reference,
        long checksumCrc32c,
        UUID aggregateId,
        long aggregateGeneration,
        String sourceOwnerId,
        long sourceOwnerLeaseGeneration,
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        String targetOwnerId,
        long targetOwnerLeaseGeneration,
        byte[] applicationPayload,
        boolean sourceCleanupCompleted,
        int terminalCompletionCount,
        int pendingRelayCount) {
        Published { applicationPayload = applicationPayload.clone(); }
        @Override public byte[] applicationPayload() {
            return applicationPayload.clone();
        }
    }
    private record Current(
        long relocationHigh,
        long relocationLow,
        long aggregateGeneration,
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        String sourceOwnerId,
        long sourceOwnerLeaseGeneration,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        String targetOwnerId,
        long targetOwnerLeaseGeneration,
        long placementReservationToken,
        String capacityOwnerId,
        long capacityOwnerLeaseGeneration,
        RoutingId capacityDescriptorRid,
        long capacityDescriptorGeneration,
        int phase,
        String reference,
        long checksumCrc32c,
        boolean sourceCleanupCompleted,
        int terminalCompletionCount,
        int pendingRelayCount) {}
    private record Owner(String ownerId, long ownerLeaseGeneration,
                         String meshName, RoutingId nodeRid,
                         long nodeGeneration) {}
    private record AuthorityShape(
        int operationKind,
        int objectKind,
        int objectState,
        int spotKind) {}
    private record Slot(
        byte[] body, int start, int end, byte[] state, boolean present) {}

    private static long crc32c(byte[] bytes) {
        return crc32c(bytes, bytes.length);
    }
    private static long crc32c(byte[] bytes, int length) {
        CRC32C crc = new CRC32C(); crc.update(bytes, 0, length); return crc.getValue();
    }
    private static IllegalArgumentException invalid() {
        return new IllegalArgumentException("invalid canonical authority payload");
    }

    private static final class Writer {
        private final ByteArrayOutputStream out = new ByteArrayOutputStream();
        void u8(long v) { out.write((int) v); }
        void u16(long v) { out.write((int) (v >>> 8)); out.write((int) v); }
        void u32(long v) { raw(ByteBuffer.allocate(4).order(ByteOrder.BIG_ENDIAN).putInt((int) v).array()); }
        void u64(long v) { raw(ByteBuffer.allocate(8).order(ByteOrder.BIG_ENDIAN).putLong(v).array()); }
        void text8(String v) { sized8(textBytes(v, 0xff)); }
        void text16(String v) { byte[] b=textBytes(v, 0xffff); u16(b.length); raw(b); }
        void sized8(byte[] b) { if (b.length<1||b.length>255) throw invalid(); u8(b.length); raw(b); }
        void raw(byte[] b) { out.writeBytes(b); }
        int size() { return out.size(); }
        byte[] bytes() { return out.toByteArray(); }

        private static byte[] textBytes(String value, int maximum) {
            Objects.requireNonNull(value, "text");
            if (value.isEmpty() || value.indexOf('\0') >= 0) {
                throw invalid();
            }
            byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
            if (bytes.length > maximum) {
                throw invalid();
            }
            return bytes;
        }
    }
    private static final class Reader {
        private final byte[] bytes; private int offset;
        Reader(byte[] bytes) { this.bytes=Objects.requireNonNull(bytes); }
        void expect(byte[] v) { for(byte b:v) if(u8()!=Byte.toUnsignedInt(b)) throw invalid(); }
        int u8(){ require(1); return Byte.toUnsignedInt(bytes[offset++]); }
        int u16(){ require(2); int v=(u8()<<8)|u8(); return v; }
        int u32(){ long v=u32Unsigned(); if(v>Integer.MAX_VALUE) throw invalid(); return (int)v; }
        long u32Unsigned(){ require(4); long v=Integer.toUnsignedLong(ByteBuffer.wrap(bytes,offset,4).order(ByteOrder.BIG_ENDIAN).getInt()); offset+=4; return v; }
        long u64(){ require(8); long v=ByteBuffer.wrap(bytes,offset,8).order(ByteOrder.BIG_ENDIAN).getLong(); offset+=8; return v; }
        long nonzeroU64(){ long v=u64(); if(v<=0) throw invalid(); return v; }
        long ordinalOrZero(){ long v=u64(); if(v<0) throw invalid(); return v; }
        String text8(){ return text(sized8()); }
        String text16(){ int n=u16(); if(n==0) throw invalid(); return text(take(n)); }
        byte[] sized8(){ int n=u8(); if(n==0) throw invalid(); return take(n); }
        Reader reader(int n){ return new Reader(take(n)); }
        void skip(int n){ take(n); }
        byte[] take(int n){ require(n); byte[] v=java.util.Arrays.copyOfRange(bytes,offset,offset+n); offset+=n; return v; }
        int offset(){ return offset; }
        int remaining(){ return bytes.length - offset; }
        byte[] bytes(){ return bytes; }
        boolean end(){ return offset==bytes.length; }
        void require(int n){ if(n<0||offset+n>bytes.length) throw invalid(); }

        private static String text(byte[] value) {
            try {
                String decoded = StandardCharsets.UTF_8.newDecoder()
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

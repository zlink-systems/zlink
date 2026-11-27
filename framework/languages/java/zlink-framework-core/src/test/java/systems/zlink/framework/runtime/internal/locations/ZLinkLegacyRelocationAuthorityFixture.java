package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.ZLinkCanonicalRelocationAuthorityStateCodec.Published;
import systems.zlink.framework.runtime.internal.locations.ZLinkCanonicalRelocationAuthorityStateCodec.State;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Objects;
import java.util.zip.CRC32C;

/** Writes previous-format fixtures; production has no binary relocation writer. */
final class ZLinkLegacyRelocationAuthorityFixture {
    private ZLinkLegacyRelocationAuthorityFixture() {}

    static byte[] publish(
            byte[] payload,
            ZLinkAggregateRelocationCoordinator.Request request,
            ZLinkAuthorityGenerationTransition transition) {
        var previous = ZLinkCanonicalRelocationAuthorityStateCodec.decode(payload);
        byte[] application =
                ZLinkCanonicalRelocationAuthorityStateCodec.applicationPayloadOrOriginal(payload);
        if (previous == null && !Arrays.equals(payload, application)) throw invalid();
        var root = ZLinkServiceRelocationEnvelopeCodec.decode(request.root());
        if (root.relocationHigh() != request.aggregateId().getMostSignificantBits()
                || root.relocationLow() != request.aggregateId().getLeastSignificantBits())
            throw invalid();
        if (previous != null) validateSuccessor(previous, request);
        Owner source =
                previous == null
                        ? owner(application)
                        : new Owner(
                                previous.sourceOwnerId(),
                                previous.sourceOwnerLeaseGeneration(),
                                previous.sourceNodeRid(),
                                previous.sourceNodeGeneration());
        byte[] projected =
                ZLinkAggregateRelocationCoordinator.projectAuthorityPayload(
                        application,
                        request.targetDescriptor(),
                        request.targetDescriptorLifecycleGeneration(),
                        request.targetOwner(),
                        transition);
        return withRelocationSlot(
                projected,
                encodeState(
                        new State(
                                root.relocationHigh(),
                                root.relocationLow(),
                                request.aggregateGeneration(),
                                request.targetAttemptGeneration(),
                                request.relocationReference(),
                                request.relocationChecksumCrc32c(),
                                source.nodeRid(),
                                source.nodeGeneration(),
                                source.ownerId(),
                                source.leaseGeneration(),
                                request.targetDescriptor().rid(),
                                request.targetDescriptorLifecycleGeneration(),
                                request.targetOwner().ownerId(),
                                request.targetOwner().leaseGeneration(),
                                source.ownerId(),
                                source.leaseGeneration(),
                                source.nodeRid(),
                                source.nodeGeneration(),
                                request.coordinatorExpectedStoreVersion(),
                                3,
                                root.applicationVersion(),
                                0)));
    }

    private static Owner owner(byte[] payload) {
        var actor = new ZLinkActorAuthorityPayloadCodec().decode(payload);
        if (actor.isPresent()) {
            var value = actor.orElseThrow();
            return new Owner(
                    value.ownerId(),
                    value.ownerLeaseGeneration(),
                    value.nodeRid(),
                    value.nodeGeneration());
        }
        var spot =
                new ZLinkServiceAuthorityPayloadCodec()
                        .decode(payload)
                        .orElseThrow(ZLinkLegacyRelocationAuthorityFixture::invalid);
        return new Owner(
                spot.ownerId(), spot.ownerLeaseGeneration(), spot.nodeRid(), spot.nodeGeneration());
    }

    private record Owner(
            String ownerId, long leaseGeneration, RoutingId nodeRid, long nodeGeneration) {}

    static byte[] withRelocationSlot(byte[] payload, byte[] state) {
        int offset = 13 + Short.toUnsignedInt(ByteBuffer.wrap(payload, 13, 2).getShort()) + 2;
        offset += 1 + Byte.toUnsignedInt(payload[offset]);
        offset += 8;
        offset += 1 + Byte.toUnsignedInt(payload[offset]);
        offset += 1 + Byte.toUnsignedInt(payload[offset]);
        offset += 8;
        int tail = offset + 5 + ByteBuffer.wrap(payload, offset + 1, 4).getInt();
        byte[] result = new byte[payload.length - (tail - offset) + state.length];
        System.arraycopy(payload, 0, result, 0, offset);
        System.arraycopy(state, 0, result, offset, state.length);
        System.arraycopy(payload, tail, result, offset + state.length, payload.length - 4 - tail);
        ByteBuffer.wrap(result, 7, 4).putInt(result.length - 15);
        CRC32C crc = new CRC32C();
        crc.update(result, 0, result.length - 4);
        ByteBuffer.wrap(result, result.length - 4, 4).putInt((int) crc.getValue());
        return result;
    }

    private static IllegalArgumentException invalid() {
        return new IllegalArgumentException("Invalid legacy relocation fixture");
    }

    static byte[] encodeState(State value) {
        Objects.requireNonNull(value, "value");
        Writer body = new Writer();
        body.u64(value.relocationHigh());
        body.u64(value.relocationLow());
        body.u64(value.aggregateGeneration());
        body.u64(value.targetAttemptGeneration());
        body.text16(value.relocationReference());
        body.u32(value.relocationChecksumCrc32c());
        body.sized8(value.sourceNodeRid().toBytes());
        body.u64(value.sourceNodeGeneration());
        body.text8(value.sourceOwnerId());
        body.u64(value.sourceOwnerLeaseGeneration());
        body.optionalSized8(value.targetNodeRid().toBytes());
        body.u64(value.targetNodeGeneration());
        body.optionalText8(value.targetOwnerId());
        body.u64(value.targetOwnerLeaseGeneration());
        body.text8(value.coordinatorOwnerId());
        body.u64(value.coordinatorLeaseGeneration());
        body.sized8(value.coordinatorNodeRid().toBytes());
        body.u64(value.coordinatorNodeGeneration());
        body.optionalText8(value.coordinatorExpectedStoreVersion());
        body.u8(value.phase());
        body.u64(value.applicationVersion());
        body.u8(value.sourceCleanupState());
        Writer state = new Writer();
        state.u8(1);
        state.u32(body.size());
        state.raw(body.bytes());
        return state.bytes();
    }

    private static void validateSuccessor(
            Published previous, ZLinkAggregateRelocationCoordinator.Request request) {
        // Successor aggregate generations are derived from the canonical root
        // and cleanup state, so they are not an ordered counter. The stable
        // relocation identity and target owner fence are the continuity check.
        if (!previous.aggregateId().equals(request.aggregateId())
                || !previous.targetOwnerId().equals(request.targetOwner().ownerId())
                || previous.targetOwnerLeaseGeneration() != request.targetOwner().leaseGeneration()
                || !previous.targetNodeRid().equals(request.targetDescriptor().rid())
                || previous.targetNodeGeneration()
                        != request.targetDescriptorLifecycleGeneration()) {
            throw new IllegalArgumentException("canonical relocation successor fence differs");
        }
    }

    private static final class Writer {
        private final ByteArrayOutputStream out = new ByteArrayOutputStream();

        void u8(long v) {
            out.write((int) v);
        }

        void u16(long v) {
            out.write((int) (v >>> 8));
            out.write((int) v);
        }

        void u32(long v) {
            raw(ByteBuffer.allocate(4).order(ByteOrder.BIG_ENDIAN).putInt((int) v).array());
        }

        void u64(long v) {
            raw(ByteBuffer.allocate(8).order(ByteOrder.BIG_ENDIAN).putLong(v).array());
        }

        void text8(String v) {
            sized8(textBytes(v, 0xff));
        }

        void text16(String v) {
            byte[] b = textBytes(v, 4096);
            u16(b.length);
            raw(b);
        }

        void optionalText8(String v) {
            byte[] b = optionalTextBytes(v, 0xff);
            u8(b.length);
            raw(b);
        }

        void sized8(byte[] b) {
            if (b.length < 1 || b.length > 255) throw invalid();
            u8(b.length);
            raw(b);
        }

        void optionalSized8(byte[] b) {
            if (b.length > 255) throw invalid();
            u8(b.length);
            raw(b);
        }

        void raw(byte[] b) {
            out.writeBytes(b);
        }

        int size() {
            return out.size();
        }

        byte[] bytes() {
            return out.toByteArray();
        }

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

        private static byte[] optionalTextBytes(String value, int maximum) {
            Objects.requireNonNull(value, "text");
            if (value.indexOf('\0') >= 0) {
                throw invalid();
            }
            byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
            if (bytes.length > maximum) {
                throw invalid();
            }
            return bytes;
        }
    }
}

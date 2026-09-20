package systems.zlink.framework.runtime.internal.service;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Objects;
import java.util.Optional;
import java.util.zip.CRC32C;
import systems.zlink.contracts.core.RoutingId;

/** Decodes the canonical durable Instance activation recovery envelope. */
public final class ZLinkInstanceActivationRecoveryCodec {
    private static final byte[] MAGIC = {0x5a, 0x4c, 0x49, 0x41};
    private static final int MAX_ENCODED_BYTES = 1024 * 1024;

    public RecoveryEnvelope decode(byte[] encoded) {
        Objects.requireNonNull(encoded, "encoded");
        if (encoded.length > MAX_ENCODED_BYTES) {
            throw invalid();
        }
        Reader reader = new Reader(encoded);
        reader.expect(MAGIC);
        if (reader.u8() != 1 || reader.u16() != 0) {
            throw invalid();
        }
        Reader body = reader.reader(reader.u32());
        int checksumOffset = reader.offset();
        long checksum = reader.unsignedU32();
        CRC32C crc = new CRC32C();
        crc.update(encoded, 0, checksumOffset);
        if (!reader.end() || checksum != crc.getValue()) {
            throw invalid();
        }

        String targetSpotId = body.text8();
        String stableType = body.text8();
        String targetMeshName = body.text8();
        RoutingId targetNodeRid = RoutingId.from(body.text8());
        long targetNodeGeneration = body.nonzeroU64();
        String descriptorVersion = body.text8();
        RoutingId sourceNodeRid = RoutingId.from(body.text8());
        long sourceNodeGeneration = body.nonzeroU64();
        Optional<String> sourceSpotId = body.optionalText8();
        int operationKind = body.u8();
        if (operationKind != 1 && operationKind != 2) {
            throw invalid();
        }
        long operationHigh = body.u64();
        long operationLow = body.u64();
        if (operationHigh == 0 && operationLow == 0) {
            throw invalid();
        }
        Long replyRouteId = operationKind == 2 ? body.nonzeroU64() : null;
        long deadlineUnixMs = body.nonzeroU64();
        int hasMetadata = body.u8();
        byte[] metadata = switch (hasMetadata) {
            case 0 -> new byte[0];
            case 1 -> body.metadataFrame();
            default -> throw invalid();
        };
        byte[] applicationPayload = body.remaining();
        if (!body.end()) {
            throw invalid();
        }
        new ZLinkServiceM6AWireCodec().decodeApplicationPayload(
            applicationPayload);
        return new RecoveryEnvelope(
            targetSpotId, stableType, targetMeshName, targetNodeRid,
            targetNodeGeneration, descriptorVersion, sourceNodeRid,
            sourceNodeGeneration, sourceSpotId, operationKind == 2,
            operationHigh, operationLow, replyRouteId, deadlineUnixMs,
            metadata, applicationPayload);
    }

    public record RecoveryEnvelope(
        String targetSpotId,
        String stableType,
        String targetMeshName,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        String descriptorVersion,
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        Optional<String> sourceSpotId,
        boolean request,
        long operationHigh,
        long operationLow,
        Long replyRouteId,
        long deadlineUnixMs,
        byte[] metadataFrame,
        byte[] applicationPayloadFrame) {
        public RecoveryEnvelope {
            sourceSpotId = Objects.requireNonNull(sourceSpotId, "sourceSpotId");
            metadataFrame = Objects.requireNonNull(metadataFrame, "metadataFrame").clone();
            applicationPayloadFrame = Objects.requireNonNull(
                applicationPayloadFrame, "applicationPayloadFrame").clone();
        }

        @Override
        public byte[] metadataFrame() {
            return metadataFrame.clone();
        }

        @Override
        public byte[] applicationPayloadFrame() {
            return applicationPayloadFrame.clone();
        }
    }

    private static IllegalArgumentException invalid() {
        return new IllegalArgumentException(
            "invalid Instance activation recovery envelope");
    }

    private static final class Reader {
        private final byte[] bytes;
        private int offset;

        Reader(byte[] bytes) {
            this.bytes = Objects.requireNonNull(bytes, "bytes");
        }

        int offset() {
            return offset;
        }

        boolean end() {
            return offset == bytes.length;
        }

        void expect(byte[] expected) {
            if (!Arrays.equals(expected, bytes(expected.length))) {
                throw invalid();
            }
        }

        int u8() {
            require(1);
            return Byte.toUnsignedInt(bytes[offset++]);
        }

        int u16() {
            require(2);
            int value = Byte.toUnsignedInt(bytes[offset]) << 8
                | Byte.toUnsignedInt(bytes[offset + 1]);
            offset += 2;
            return value;
        }

        int u32() {
            long value = unsignedU32();
            if (value > Integer.MAX_VALUE) throw invalid();
            return (int) value;
        }

        long unsignedU32() {
            require(4);
            long value = Integer.toUnsignedLong(ByteBuffer.wrap(bytes, offset, 4)
                .order(ByteOrder.BIG_ENDIAN).getInt());
            offset += 4;
            return value;
        }

        long u64() {
            require(8);
            long value = ByteBuffer.wrap(bytes, offset, 8)
                .order(ByteOrder.BIG_ENDIAN).getLong();
            offset += 8;
            return value;
        }

        long nonzeroU64() {
            long value = u64();
            if (value == 0) throw invalid();
            return value;
        }

        String text8() {
            int length = u8();
            if (length == 0) throw invalid();
            byte[] value = bytes(length);
            for (byte item : value) {
                if (item == 0) throw invalid();
            }
            try {
                return StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(ByteBuffer.wrap(value)).toString();
            } catch (CharacterCodingException failure) {
                throw invalid();
            }
        }

        Optional<String> optionalText8() {
            int present = u8();
            if (present == 0) return Optional.empty();
            if (present != 1) throw invalid();
            return Optional.of(text8());
        }

        byte[] metadataFrame() {
            int start = offset;
            if (u8() != 1) throw invalid();
            int count = u8();
            for (int index = 0; index < count; index++) {
                bytes(u8());
                bytes(u16());
            }
            return Arrays.copyOfRange(bytes, start, offset);
        }

        Reader reader(int length) {
            return new Reader(bytes(length));
        }

        byte[] remaining() {
            return bytes(bytes.length - offset);
        }

        byte[] bytes(int length) {
            require(length);
            byte[] value = Arrays.copyOfRange(bytes, offset, offset + length);
            offset += length;
            return value;
        }

        void require(int length) {
            if (length < 0 || offset + length > bytes.length) throw invalid();
        }
    }
}

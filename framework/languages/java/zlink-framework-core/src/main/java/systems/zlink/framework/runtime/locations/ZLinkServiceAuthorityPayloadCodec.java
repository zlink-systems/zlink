package systems.zlink.framework.runtime.locations;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Objects;
import java.util.Optional;
import java.util.zip.CRC32C;
import systems.zlink.contracts.core.RoutingId;

/**
 * Closed decoder for the durable authority payload shared by all runtimes.
 * Invalid or non-Spot payloads never enter the raw route cache.
 */
public final class ZLinkServiceAuthorityPayloadCodec {
    private static final byte[] MAGIC = {0x5a, 0x4c, 0x41, 0x55};

    public enum Kind {
        USER,
        INSTANCE
    }

    public enum State {
        CREATING,
        READY,
        CLOSING
    }

    public record SpotAuthority(
        Kind kind,
        State state,
        String stableType,
        String spotId,
        String ownerId,
        long ownerLeaseGeneration,
        String meshName,
        RoutingId nodeRid,
        long nodeGeneration) {
    }

    public Optional<SpotAuthority> decode(byte[] payload) {
        try {
            byte[] authorityPayload =
                systems.zlink.framework.runtime.internal.locations
                    .ZLinkCanonicalRelocationAuthorityStateCodec
                    .applicationPayloadOrOriginal(payload);
            Reader reader = new Reader(authorityPayload);
            reader.expect(MAGIC);
            if (reader.u8() != 1 || reader.u16() != 0) {
                return Optional.empty();
            }
            int bodyLength = reader.u32();
            Reader body = reader.reader(bodyLength);
            int checksumOffset = reader.position();
            long checksum = reader.unsignedU32();
            if (!reader.end()
                || checksum != crc32c(authorityPayload, checksumOffset)) {
                return Optional.empty();
            }

            int operationKind = body.u8();
            int objectKind = body.u8();
            Reader object = body.reader(body.u16());
            if (objectKind != 2) {
                return Optional.empty();
            }
            int spotKind = object.u8();
            Reader spot = object.reader(object.u16());
            Kind kind;
            State state;
            String spotId;
            String stableType;
            if (spotKind == 2) {
                kind = Kind.USER;
                spotId = spot.text8();
                stableType = spot.text8();
                int userState = spot.u8();
                state = userState == 0 && operationKind == 1
                    ? State.CREATING
                    : userState == 1 && operationKind == 0
                        ? State.READY
                        : userState == 2 && operationKind == 3
                            ? State.CLOSING
                            : null;
            } else if (spotKind == 3) {
                kind = Kind.INSTANCE;
                int instanceState = spot.u8();
                Reader instance = spot.reader(spot.u16());
                stableType = instance.text8();
                spotId = instance.text8();
                if (!instance.end()) {
                    return Optional.empty();
                }
                state = instanceState == 1 && operationKind == 1
                    ? State.CREATING
                    : instanceState == 2 && operationKind == 0
                        ? State.READY
                        : instanceState == 3 && operationKind == 3
                            ? State.CLOSING
                            : null;
            } else {
                return Optional.empty();
            }
            if (state == null || !spot.end() || !object.end()) {
                return Optional.empty();
            }
            String ownerId = body.text8();
            long ownerLeaseGeneration = body.nonzeroU64();
            String meshName = body.text8();
            RoutingId nodeRid = body.rid();
            long nodeGeneration = body.nonzeroU64();
            if (!emptyConditional32(body)) {
                return Optional.empty();
            }
            skipActivationRecovery(body, kind, state, operationKind);
            if (!body.end()) {
                return Optional.empty();
            }
            return Optional.of(new SpotAuthority(
                kind,
                state,
                stableType,
                spotId,
                ownerId,
                ownerLeaseGeneration,
                meshName,
                nodeRid,
                nodeGeneration));
        } catch (RuntimeException invalid) {
            return Optional.empty();
        }
    }

    public byte[] encodeUser(
        State state,
        String stableType,
        String spotId,
        String ownerId,
        long ownerLeaseGeneration,
        String meshName,
        RoutingId nodeRid,
        long nodeGeneration) {
        Writer spot = new Writer();
        spot.text8(systems.zlink.framework.runtime.internal.spots
            .ZLinkSpotIdValidator.requireValid(spotId));
        spot.text8(stableType);
        spot.u8(switch (state) {
            case CREATING -> 0;
            case READY -> 1;
            case CLOSING -> 2;
        });
        Writer object = new Writer();
        object.conditional16(2, spot.bytes());
        Writer body = new Writer();
        body.u8(switch (state) {
            case CREATING -> 1;
            case READY -> 0;
            case CLOSING -> 3;
        });
        body.conditional16(2, object.bytes());
        body.text8(ownerId);
        body.nonzeroU64(ownerLeaseGeneration);
        body.text8(meshName);
        body.rid(nodeRid);
        body.nonzeroU64(nodeGeneration);
        body.conditional32(0, new byte[0]);
        body.conditional32(0, new byte[0]);
        Writer envelope = new Writer();
        envelope.raw(MAGIC);
        envelope.u8(1);
        envelope.u16(0);
        envelope.u32(body.size());
        envelope.raw(body.bytes());
        byte[] withoutChecksum = envelope.bytes();
        envelope.u32(crc32c(
            withoutChecksum, withoutChecksum.length));
        return envelope.bytes();
    }

    public byte[] encodeInstance(
        State state,
        String stableType,
        String spotId,
        String ownerId,
        long ownerLeaseGeneration,
        String meshName,
        RoutingId nodeRid,
        long nodeGeneration) {
        Writer instance = new Writer();
        instance.text8(stableType);
        instance.text8(systems.zlink.framework.runtime.internal.spots
            .ZLinkSpotIdValidator.requireValid(spotId));
        Writer spot = new Writer();
        spot.u8(switch (state) {
            case CREATING -> 1;
            case READY -> 2;
            case CLOSING -> 3;
        });
        spot.u16(instance.size());
        spot.raw(instance.bytes());
        Writer object = new Writer();
        object.conditional16(3, spot.bytes());
        Writer body = new Writer();
        body.u8(switch (state) {
            case CREATING -> 1;
            case READY -> 0;
            case CLOSING -> 3;
        });
        body.conditional16(2, object.bytes());
        body.text8(ownerId);
        body.nonzeroU64(ownerLeaseGeneration);
        body.text8(meshName);
        body.rid(nodeRid);
        body.nonzeroU64(nodeGeneration);
        body.conditional32(0, new byte[0]);
        body.conditional32(0, new byte[0]);
        Writer envelope = new Writer();
        envelope.raw(MAGIC);
        envelope.u8(1);
        envelope.u16(0);
        envelope.u32(body.size());
        envelope.raw(body.bytes());
        byte[] withoutChecksum = envelope.bytes();
        envelope.u32(crc32c(
            withoutChecksum, withoutChecksum.length));
        return envelope.bytes();
    }

    private static boolean emptyConditional32(Reader reader) {
        return reader.u8() == 0
            && reader.u32() == 0;
    }

    private static void skipActivationRecovery(
        Reader body,
        Kind kind,
        State state,
        int operationKind) {
        int present = body.u8();
        int length = body.u32();
        Reader recovery = body.reader(length);
        if (present == 0) {
            if (length != 0 || !recovery.end()) {
                throw new IllegalArgumentException(
                    "invalid empty activation recovery");
            }
            return;
        }
        if (present != 1
            || kind != Kind.INSTANCE
            || state != State.READY
            || operationKind != 0) {
            throw new IllegalArgumentException(
                "invalid activation recovery discriminator");
        }
        recovery.text16();
        int digestLength = recovery.u8();
        if (digestLength != 32) {
            throw new IllegalArgumentException(
                "invalid activation recovery digest");
        }
        recovery.skip(digestLength);
        long encodedSize = recovery.unsignedU32();
        long inboxSequence = recovery.nonzeroU64();
        long replayCursor = recovery.u64();
        if (encodedSize > 1024 * 1024
            || replayCursor > inboxSequence
            || !recovery.end()) {
            throw new IllegalArgumentException(
                "invalid activation recovery");
        }
    }

    private static long crc32c(byte[] value, int length) {
        CRC32C checksum = new CRC32C();
        checksum.update(value, 0, length);
        return checksum.getValue();
    }

    private static final class Writer {
        private final ByteArrayOutputStream output =
            new ByteArrayOutputStream();

        void u8(int value) {
            output.write(value);
        }

        void u16(int value) {
            output.write((value >>> 8) & 0xff);
            output.write(value & 0xff);
        }

        void u32(long value) {
            raw(ByteBuffer.allocate(4)
                .order(ByteOrder.BIG_ENDIAN)
                .putInt((int) value)
                .array());
        }

        void nonzeroU64(long value) {
            if (value <= 0) {
                throw new IllegalArgumentException(
                    "authority generation must be positive");
            }
            raw(ByteBuffer.allocate(8)
                .order(ByteOrder.BIG_ENDIAN)
                .putLong(value)
                .array());
        }

        void text8(String value) {
            byte[] bytes = bounded(value);
            u8(bytes.length);
            raw(bytes);
        }

        void rid(RoutingId value) {
            byte[] bytes = Objects.requireNonNull(value, "rid")
                .toBytes();
            if (bytes.length == 0 || bytes.length > 0xff) {
                throw new IllegalArgumentException(
                    "authority RID exceeds bounds");
            }
            u8(bytes.length);
            raw(bytes);
        }

        void conditional16(int discriminator, byte[] body) {
            u8(discriminator);
            u16(body.length);
            raw(body);
        }

        void conditional32(int discriminator, byte[] body) {
            u8(discriminator);
            u32(body.length);
            raw(body);
        }

        void raw(byte[] value) {
            output.writeBytes(value);
        }

        int size() {
            return output.size();
        }

        byte[] bytes() {
            return output.toByteArray();
        }

        private static byte[] bounded(String value) {
            byte[] bytes = Objects.requireNonNull(value, "text")
                .getBytes(StandardCharsets.UTF_8);
            if (bytes.length == 0
                || bytes.length > 0xff
                || value.indexOf('\0') >= 0) {
                throw new IllegalArgumentException(
                    "authority text exceeds bounds");
            }
            return bytes;
        }
    }

    private static final class Reader {
        private final byte[] bytes;
        private int offset;

        Reader(byte[] bytes) {
            this.bytes = Objects.requireNonNull(bytes, "bytes");
        }

        void expect(byte[] expected) {
            for (byte value : expected) {
                if (u8() != Byte.toUnsignedInt(value)) {
                    throw new IllegalArgumentException(
                        "authority magic mismatch");
                }
            }
        }

        int u8() {
            require(1);
            return Byte.toUnsignedInt(bytes[offset++]);
        }

        int u16() {
            require(2);
            int result =
                Byte.toUnsignedInt(bytes[offset]) << 8
                    | Byte.toUnsignedInt(bytes[offset + 1]);
            offset += 2;
            return result;
        }

        int u32() {
            long value = unsignedU32();
            if (value > Integer.MAX_VALUE) {
                throw new IllegalArgumentException(
                    "authority length exceeds JVM bound");
            }
            return (int) value;
        }

        long unsignedU32() {
            require(4);
            long result = Integer.toUnsignedLong(
                ByteBuffer.wrap(bytes, offset, 4)
                    .order(ByteOrder.BIG_ENDIAN)
                    .getInt());
            offset += 4;
            return result;
        }

        long nonzeroU64() {
            long value = u64();
            if (value == 0) {
                throw new IllegalArgumentException(
                    "authority nonzero u64 is zero");
            }
            return value;
        }

        long u64() {
            require(8);
            long value = ByteBuffer.wrap(bytes, offset, 8)
                .order(ByteOrder.BIG_ENDIAN)
                .getLong();
            offset += 8;
            if (value < 0) {
                throw new IllegalArgumentException(
                    "authority u64 exceeds JVM bound");
            }
            return value;
        }

        RoutingId rid() {
            int length = u8();
            if (length == 0) {
                throw new IllegalArgumentException(
                    "authority RID is empty");
            }
            require(length);
            byte[] value =
                java.util.Arrays.copyOfRange(
                    bytes, offset, offset + length);
            offset += length;
            return RoutingId.from(value);
        }

        String text8() {
            return text(u8());
        }

        String text16() {
            return text(u16());
        }

        String text(int length) {
            if (length == 0) {
                throw new IllegalArgumentException(
                    "authority text is empty");
            }
            require(length);
            ByteBuffer value =
                ByteBuffer.wrap(bytes, offset, length);
            offset += length;
            try {
                String decoded = StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(value)
                    .toString();
                if (decoded.indexOf('\0') >= 0) {
                    throw new IllegalArgumentException(
                        "authority text contains NUL");
                }
                return decoded;
            } catch (CharacterCodingException invalid) {
                throw new IllegalArgumentException(
                    "authority text is not UTF-8", invalid);
            }
        }

        Reader reader(int length) {
            require(length);
            Reader result = new Reader(
                java.util.Arrays.copyOfRange(
                    bytes, offset, offset + length));
            offset += length;
            return result;
        }

        void skip(int length) {
            require(length);
            offset += length;
        }

        int position() {
            return offset;
        }

        boolean end() {
            return offset == bytes.length;
        }

        private void require(int length) {
            if (length < 0 || offset + length > bytes.length) {
                throw new IllegalArgumentException(
                    "authority payload is truncated");
            }
        }
    }
}

package systems.zlink.framework.runtime.locations;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.Objects;
import java.util.Optional;
import java.util.zip.CRC32C;
import systems.zlink.contracts.core.RoutingId;

/**
 * Encodes the Actor branch of the durable service authority envelope.
 */
public final class ZLinkActorAuthorityPayloadCodec {
    private static final byte[] MAGIC = {0x5a, 0x4c, 0x41, 0x55};

    public enum State {
        CREATING,
        READY
    }

    public record ActorAuthority(
        State state,
        String stableType,
        String actorId,
        String currentSpotId,
        long currentSpotGeneration,
        int currentSpotKind,
        String ownerId,
        long ownerLeaseGeneration,
        String meshName,
        RoutingId nodeRid,
        long nodeGeneration) {
    }

    public byte[] encode(
        State state,
        String stableType,
        String actorId,
        String currentSpotId,
        long currentSpotGeneration,
        int currentSpotKind,
        String ownerId,
        long ownerLeaseGeneration,
        String meshName,
        RoutingId nodeRid,
        long nodeGeneration) {
        Writer actor = new Writer();
        actor.text8(stableType);
        actor.text8(actorId);
        actor.u8(state == State.CREATING ? 0 : 1);
        actor.text8(currentSpotId);
        actor.nonzeroU64(currentSpotGeneration);
        actor.u8(currentSpotKind);

        Writer body = new Writer();
        body.u8(state == State.CREATING ? 1 : 0);
        body.conditional16(1, actor.bytes());
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
        envelope.u32(crc32c(withoutChecksum, withoutChecksum.length));
        return envelope.bytes();
    }

    public Optional<ActorAuthority> decode(byte[] payload) {
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
            Reader body = reader.reader(reader.u32());
            int checksumOffset = reader.position();
            long checksum = reader.u32();
            if (!reader.end()
                || checksum != crc32c(authorityPayload, checksumOffset)) {
                return Optional.empty();
            }
            int operationKind = body.u8();
            if (body.u8() != 1) {
                return Optional.empty();
            }
            Reader object = body.reader(body.u16());
            String stableType = object.text8();
            String actorId = object.text8();
            int stateValue = object.u8();
            String currentSpotId = object.text8();
            long currentSpotGeneration = object.nonzeroU64();
            int currentSpotKind = object.u8();
            if (!object.end()) {
                return Optional.empty();
            }
            State state = stateValue == 0 && operationKind == 1
                ? State.CREATING
                : stateValue == 1 && operationKind == 0
                    ? State.READY
                    : null;
            if (state == null
                || (currentSpotKind != 1 && currentSpotKind != 2)) {
                return Optional.empty();
            }
            String ownerId = body.text8();
            long ownerLeaseGeneration = body.nonzeroU64();
            String meshName = body.text8();
            RoutingId nodeRid = body.rid();
            long nodeGeneration = body.nonzeroU64();
            if (!body.emptyConditional32()
                || !body.emptyConditional32()
                || !body.end()) {
                return Optional.empty();
            }
            return Optional.of(new ActorAuthority(
                state,
                stableType,
                actorId,
                currentSpotId,
                currentSpotGeneration,
                currentSpotKind,
                ownerId,
                ownerLeaseGeneration,
                meshName,
                nodeRid,
                nodeGeneration));
        } catch (RuntimeException invalid) {
            return Optional.empty();
        }
    }

    private static long crc32c(byte[] bytes, int length) {
        CRC32C checksum = new CRC32C();
        checksum.update(bytes, 0, length);
        return checksum.getValue();
    }

    private static final class Writer {
        private final ByteArrayOutputStream output =
            new ByteArrayOutputStream();

        void raw(byte[] value) {
            output.writeBytes(value);
        }

        void u8(int value) {
            if (value < 0 || value > 0xff) {
                throw new IllegalArgumentException("u8 out of range");
            }
            output.write(value);
        }

        void u16(int value) {
            if (value < 0 || value > 0xffff) {
                throw new IllegalArgumentException("u16 out of range");
            }
            output.write((value >>> 8) & 0xff);
            output.write(value & 0xff);
        }

        void u32(long value) {
            if (value < 0 || value > 0xffff_ffffL) {
                throw new IllegalArgumentException("u32 out of range");
            }
            raw(ByteBuffer.allocate(4).order(ByteOrder.BIG_ENDIAN)
                .putInt((int) value).array());
        }

        void nonzeroU64(long value) {
            if (value <= 0) {
                throw new IllegalArgumentException(
                    "nonzero-u64 must be positive");
            }
            raw(ByteBuffer.allocate(8).order(ByteOrder.BIG_ENDIAN)
                .putLong(value).array());
        }

        void text8(String value) {
            byte[] bytes = Objects.requireNonNull(value, "text")
                .getBytes(StandardCharsets.UTF_8);
            if (bytes.length == 0 || bytes.length > 0xff
                || value.indexOf('\0') >= 0) {
                throw new IllegalArgumentException("text8 out of range");
            }
            u8(bytes.length);
            raw(bytes);
        }

        void rid(RoutingId value) {
            byte[] bytes = Objects.requireNonNull(value, "rid").toBytes();
            if (bytes.length == 0 || bytes.length > 0xff) {
                throw new IllegalArgumentException("rid out of range");
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

        int size() {
            return output.size();
        }

        byte[] bytes() {
            return output.toByteArray();
        }
    }

    private static final class Reader {
        private final ByteBuffer input;

        Reader(byte[] value) {
            input = ByteBuffer.wrap(
                Objects.requireNonNull(value, "value"))
                .order(ByteOrder.BIG_ENDIAN);
        }

        int position() {
            return input.position();
        }

        int u8() {
            require(1);
            return Byte.toUnsignedInt(input.get());
        }

        int u16() {
            require(2);
            return Short.toUnsignedInt(input.getShort());
        }

        long u32() {
            require(4);
            return Integer.toUnsignedLong(input.getInt());
        }

        long nonzeroU64() {
            require(8);
            long value = input.getLong();
            if (value <= 0) {
                throw new IllegalArgumentException("invalid nonzero-u64");
            }
            return value;
        }

        String text8() {
            int length = u8();
            require(length);
            byte[] bytes = new byte[length];
            input.get(bytes);
            String value = new String(bytes, StandardCharsets.UTF_8);
            if (value.indexOf('\0') >= 0) {
                throw new IllegalArgumentException("invalid text8");
            }
            return value;
        }

        RoutingId rid() {
            int length = u8();
            require(length);
            byte[] bytes = new byte[length];
            input.get(bytes);
            return RoutingId.from(bytes);
        }

        Reader reader(long length) {
            if (length > Integer.MAX_VALUE) {
                throw new IllegalArgumentException("body too large");
            }
            require((int) length);
            byte[] bytes = new byte[(int) length];
            input.get(bytes);
            return new Reader(bytes);
        }

        boolean emptyConditional32() {
            return u8() == 0 && u32() == 0;
        }

        void expect(byte[] expected) {
            require(expected.length);
            for (byte value : expected) {
                if (input.get() != value) {
                    throw new IllegalArgumentException("magic mismatch");
                }
            }
        }

        boolean end() {
            return !input.hasRemaining();
        }

        private void require(int length) {
            if (length < 0 || input.remaining() < length) {
                throw new IllegalArgumentException("truncated payload");
            }
        }
    }
}

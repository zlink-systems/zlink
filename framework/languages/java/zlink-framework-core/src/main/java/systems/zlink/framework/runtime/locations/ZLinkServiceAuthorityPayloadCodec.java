package systems.zlink.framework.runtime.locations;
import java.util.Arrays;

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

    public enum State {
        CREATING,
        READY,
        CLOSING
    }

    public sealed interface SpotAuthority
        permits UserSpotAuthority, InstanceSpotAuthority {
        State state();
        String stableType();
        String spotId();
        String ownerId();
        long ownerLeaseGeneration();
        String meshName();
        RoutingId nodeRid();
        long nodeGeneration();

        default Optional<ActivationRecoveryState> activationRecoveryState() {
            return Optional.empty();
        }

        default Optional<UserSpotAuthority> user() {
            return Optional.empty();
        }

        default Optional<InstanceSpotAuthority> instance() {
            return Optional.empty();
        }
    }

    public record UserSpotAuthority(
        State state,
        String stableType,
        String spotId,
        String ownerId,
        long ownerLeaseGeneration,
        String meshName,
        RoutingId nodeRid,
        long nodeGeneration) implements SpotAuthority {
        @Override
        public Optional<UserSpotAuthority> user() {
            return Optional.of(this);
        }
    }

    public record InstanceSpotAuthority(
        State state,
        String stableType,
        String spotId,
        String ownerId,
        long ownerLeaseGeneration,
        String meshName,
        RoutingId nodeRid,
        long nodeGeneration,
        Optional<ActivationRecoveryState> activationRecoveryState)
        implements SpotAuthority {
        public InstanceSpotAuthority {
            activationRecoveryState = Objects.requireNonNull(
                activationRecoveryState, "activationRecoveryState");
        }

        public InstanceSpotAuthority(
            State state,
            String stableType,
            String spotId,
            String ownerId,
            long ownerLeaseGeneration,
            String meshName,
            RoutingId nodeRid,
            long nodeGeneration) {
            this(
                state, stableType, spotId, ownerId, ownerLeaseGeneration,
                meshName, nodeRid, nodeGeneration, Optional.empty());
        }

        @Override
        public Optional<InstanceSpotAuthority> instance() {
            return Optional.of(this);
        }
    }

    public record ActivationRecoveryState(
        String reference,
        byte[] sha256,
        long encodedSize,
        long inboxSequence) {
        public ActivationRecoveryState {
            byte[] digest = Objects.requireNonNull(sha256, "sha256").clone();
            if (digest.length != 32
                || encodedSize < 0 || encodedSize > 1024 * 1024
                || inboxSequence <= 0) {
                throw new IllegalArgumentException(
                    "invalid activation recovery state");
            }
            validateText8(reference);
            sha256 = digest;
        }

        @Override
        public byte[] sha256() {
            return sha256.clone();
        }
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
            DecodedSpot decodedSpot;
            if (spotKind == 2) {
                String spotId = spot.text8();
                String stableType = spot.text8();
                int userState = spot.u8();
                State state = userState == 0 && operationKind == 1
                    ? State.CREATING
                    : userState == 1 && operationKind == 0
                        ? State.READY
                        : userState == 2 && operationKind == 3
                            ? State.CLOSING
                            : null;
                decodedSpot = new DecodedUserSpot(state, stableType, spotId);
            } else if (spotKind == 3) {
                int instanceState = spot.u8();
                Reader instance = spot.reader(spot.u16());
                String stableType = instance.text8();
                String spotId = instance.text8();
                if (!instance.end()) {
                    return Optional.empty();
                }
                State state = instanceState == 1 && operationKind == 1
                    ? State.CREATING
                    : instanceState == 2 && operationKind == 0
                        ? State.READY
                        : instanceState == 3 && operationKind == 3
                            ? State.CLOSING
                            : null;
                decodedSpot = new DecodedInstanceSpot(state, stableType, spotId);
            } else {
                return Optional.empty();
            }
            if (decodedSpot.state() == null || !spot.end() || !object.end()) {
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
            Optional<ActivationRecoveryState> activationRecoveryState =
                decodedSpot.activationRecoveryState(body, operationKind);
            if (!body.end()) {
                return Optional.empty();
            }
            return Optional.of(decodedSpot.authority(
                ownerId,
                ownerLeaseGeneration,
                meshName,
                nodeRid,
                nodeGeneration,
                activationRecoveryState));
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
        return encodeInstance(
            state, stableType, spotId, ownerId, ownerLeaseGeneration,
            meshName, nodeRid, nodeGeneration, Optional.empty());
    }

    public byte[] encodeInstance(
        State state,
        String stableType,
        String spotId,
        String ownerId,
        long ownerLeaseGeneration,
        String meshName,
        RoutingId nodeRid,
        long nodeGeneration,
        Optional<ActivationRecoveryState> activationRecoveryState) {
        activationRecoveryState = Objects.requireNonNull(
            activationRecoveryState, "activationRecoveryState");
        if (activationRecoveryState.isPresent() && state != State.READY) {
            throw new IllegalArgumentException(
                "activation recovery requires a ready instance authority");
        }
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
        body.conditional32(
            activationRecoveryState.isPresent() ? 1 : 0,
            activationRecoveryState.map(
                ZLinkServiceAuthorityPayloadCodec::encodeActivationRecovery)
                .orElseGet(() -> new byte[0]));
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

    public byte[] encode(SpotAuthority authority) {
        Objects.requireNonNull(authority, "authority");
        if (authority instanceof UserSpotAuthority user) {
            return encodeUser(
                user.state(), user.stableType(), user.spotId(), user.ownerId(),
                user.ownerLeaseGeneration(), user.meshName(), user.nodeRid(),
                user.nodeGeneration());
        }
        if (authority instanceof InstanceSpotAuthority instance) {
            return encodeInstance(
                instance.state(), instance.stableType(), instance.spotId(),
                instance.ownerId(), instance.ownerLeaseGeneration(),
                instance.meshName(), instance.nodeRid(), instance.nodeGeneration(),
                instance.activationRecoveryState());
        }
        throw new IllegalArgumentException("unsupported spot authority");
    }

    private static boolean emptyConditional32(Reader reader) {
        return reader.u8() == 0
            && reader.u32() == 0;
    }

    private static void skipEmptyActivationRecovery(Reader body) {
        int present = body.u8();
        int length = body.u32();
        Reader recovery = body.reader(length);
        if (present != 0 || length != 0 || !recovery.end()) {
            throw new IllegalArgumentException(
                "invalid empty activation recovery");
        }
    }

    private static Optional<ActivationRecoveryState> readInstanceActivationRecovery(
        Reader body,
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
            return Optional.empty();
        }
        if (present != 1 || state != State.READY || operationKind != 0) {
            throw new IllegalArgumentException(
                "invalid activation recovery discriminator");
        }
        String reference = recovery.text8();
        byte[] sha256 = recovery.bytes(32);
        long encodedSize = recovery.unsignedU32();
        long inboxSequence = recovery.nonzeroU64();
        if (!recovery.end()) {
            throw new IllegalArgumentException(
                "invalid activation recovery");
        }
        return Optional.of(new ActivationRecoveryState(
            reference, sha256, encodedSize, inboxSequence));
    }

    private static byte[] encodeActivationRecovery(
        ActivationRecoveryState recovery) {
        Writer writer = new Writer();
        writer.text8(recovery.reference());
        writer.raw(recovery.sha256());
        writer.u32(recovery.encodedSize());
        writer.nonzeroU64(recovery.inboxSequence());
        return writer.bytes();
    }

    private static void validateText8(String value) {
        Writer.bounded(value);
    }

    private sealed interface DecodedSpot
        permits DecodedUserSpot, DecodedInstanceSpot {
        State state();
        String stableType();
        String spotId();
        Optional<ActivationRecoveryState> activationRecoveryState(
            Reader body,
            int operationKind);
        SpotAuthority authority(
            String ownerId,
            long ownerLeaseGeneration,
            String meshName,
            RoutingId nodeRid,
            long nodeGeneration,
            Optional<ActivationRecoveryState> activationRecoveryState);
    }

    private record DecodedUserSpot(
        State state,
        String stableType,
        String spotId) implements DecodedSpot {
        @Override
        public Optional<ActivationRecoveryState> activationRecoveryState(
            Reader body,
            int operationKind) {
            skipEmptyActivationRecovery(body);
            return Optional.empty();
        }

        @Override
        public SpotAuthority authority(
            String ownerId,
            long ownerLeaseGeneration,
            String meshName,
            RoutingId nodeRid,
            long nodeGeneration,
            Optional<ActivationRecoveryState> activationRecoveryState) {
            return new UserSpotAuthority(
                state, stableType, spotId, ownerId, ownerLeaseGeneration,
                meshName, nodeRid, nodeGeneration);
        }
    }

    private record DecodedInstanceSpot(
        State state,
        String stableType,
        String spotId) implements DecodedSpot {
        @Override
        public Optional<ActivationRecoveryState> activationRecoveryState(
            Reader body,
            int operationKind) {
            return readInstanceActivationRecovery(body, state, operationKind);
        }

        @Override
        public SpotAuthority authority(
            String ownerId,
            long ownerLeaseGeneration,
            String meshName,
            RoutingId nodeRid,
            long nodeGeneration,
            Optional<ActivationRecoveryState> activationRecoveryState) {
            return new InstanceSpotAuthority(
                state, stableType, spotId, ownerId, ownerLeaseGeneration,
                meshName, nodeRid, nodeGeneration, activationRecoveryState);
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
                Arrays.copyOfRange(
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

        byte[] bytes(int length) {
            require(length);
            byte[] value = Arrays.copyOfRange(
                bytes, offset, offset + length);
            offset += length;
            return value;
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
                Arrays.copyOfRange(
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

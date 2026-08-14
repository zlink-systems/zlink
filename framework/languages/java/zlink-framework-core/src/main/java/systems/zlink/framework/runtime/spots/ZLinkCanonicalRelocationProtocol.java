package systems.zlink.framework.runtime.spots;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Objects;
import java.util.UUID;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

/** Semantic projection of the four canonical relocation controls. */
final class ZLinkCanonicalRelocationProtocol {
    private static final int ROLE_SOURCE = 1;
    private static final int ROLE_TARGET = 2;

    private ZLinkCanonicalRelocationProtocol() {
    }

    static byte[] encodePrepare(Prepare value) {
        Objects.requireNonNull(value, "value");
        Writer writer = body(ServiceWireConstants.COMMAND_RELOCATION_PREPARE);
        relocation(writer, value.id(), value.targetAttemptGeneration());
        coordinator(writer, value.coordinator());
        target(writer, value.target());
        writer.u8(value.initiatorRole());
        object(writer, value.object());
        writer.rid(value.sourceNodeRid());
        writer.nonzero(value.sourceNodeGeneration());
        root(writer, value.root());
        writer.u64(value.applicationVersion());
        return writer.bytes();
    }

    static Prepare decodePrepare(byte[] encoded) {
        Reader reader = body(encoded,
            ServiceWireConstants.COMMAND_RELOCATION_PREPARE);
        Prepare value = new Prepare(
            reader.uuid(),
            reader.nonzero(),
            coordinator(reader),
            target(reader),
            reader.role(),
            object(reader),
            reader.rid(),
            reader.nonzero(),
            root(reader),
            reader.u64());
        reader.end();
        return value;
    }

    static byte[] encodeReady(Ready value) {
        Objects.requireNonNull(value, "value");
        Writer writer = body(ServiceWireConstants.COMMAND_RELOCATION_READY);
        relocation(writer, value.id(), value.targetAttemptGeneration());
        coordinator(writer, value.coordinator());
        target(writer, value.target());
        object(writer, value.object());
        writer.u8(value.senderRole());
        return writer.bytes();
    }

    static Ready decodeReady(byte[] encoded) {
        Reader reader = body(encoded,
            ServiceWireConstants.COMMAND_RELOCATION_READY);
        Ready value = new Ready(
            reader.uuid(),
            reader.nonzero(),
            coordinator(reader),
            target(reader),
            object(reader),
            reader.role());
        reader.end();
        return value;
    }

    static byte[] encodeData(Data value) {
        Objects.requireNonNull(value, "value");
        Writer writer = body(ServiceWireConstants.COMMAND_RELOCATION_DATA);
        relocation(writer, value.id(), value.targetAttemptGeneration());
        coordinator(writer, value.coordinator());
        writer.u8(value.senderRole());
        object(writer, value.object());
        writer.raw(value.frozenRecord());
        return writer.bytes();
    }

    static Data decodeData(byte[] encoded) {
        Reader reader = body(encoded,
            ServiceWireConstants.COMMAND_RELOCATION_DATA);
        Data value = new Data(
            reader.uuid(),
            reader.nonzero(),
            coordinator(reader),
            reader.role(),
            object(reader),
            reader.remainingBytes());
        reader.end();
        return value;
    }

    static byte[] encodeCutover(Cutover value) {
        Objects.requireNonNull(value, "value");
        Writer writer = body(ServiceWireConstants.COMMAND_RELOCATION_CUTOVER);
        relocation(writer, value.id(), value.targetAttemptGeneration());
        coordinator(writer, value.coordinator());
        writer.u8(value.senderRole());
        object(writer, value.object());
        return writer.bytes();
    }

    static Cutover decodeCutover(byte[] encoded) {
        Reader reader = body(encoded,
            ServiceWireConstants.COMMAND_RELOCATION_CUTOVER);
        Cutover value = new Cutover(
            reader.uuid(),
            reader.nonzero(),
            coordinator(reader),
            reader.role(),
            object(reader));
        reader.end();
        return value;
    }

    private static Writer body(int command) {
        Writer writer = new Writer();
        writer.u8(ServiceWireConstants.MAGIC_0);
        writer.u8(ServiceWireConstants.MAGIC_1);
        writer.u8(ServiceWireConstants.WIRE_MAJOR);
        writer.u8(command);
        writer.u8(0);
        return writer;
    }

    private static Reader body(byte[] encoded, int command) {
        Reader reader = new Reader(encoded);
        if (reader.u8() != ServiceWireConstants.MAGIC_0
            || reader.u8() != ServiceWireConstants.MAGIC_1
            || reader.u8() != ServiceWireConstants.WIRE_MAJOR
            || reader.u8() != command
            || reader.u8() != 0) {
            throw invalid("prefix");
        }
        return reader;
    }

    private static void relocation(
        Writer writer, UUID id, long targetAttemptGeneration) {
        writer.uuid(id);
        writer.nonzero(targetAttemptGeneration);
    }

    private static void coordinator(Writer writer, Coordinator value) {
        writer.text8(value.ownerId());
        writer.nonzero(value.ownerLeaseGeneration());
        writer.rid(value.nodeRid());
        writer.nonzero(value.nodeGeneration());
        writer.text16(value.expectedAuthorityStoreVersion());
    }

    private static Coordinator coordinator(Reader reader) {
        return new Coordinator(
            reader.text8(),
            reader.nonzero(),
            reader.rid(),
            reader.nonzero(),
            reader.text16());
    }

    private static void target(Writer writer, Target value) {
        writer.rid(value.nodeRid());
        writer.nonzero(value.nodeGeneration());
        writer.text8(value.ownerId());
        writer.nonzero(value.ownerLeaseGeneration());
    }

    private static Target target(Reader reader) {
        return new Target(
            reader.rid(),
            reader.nonzero(),
            reader.text8(),
            reader.nonzero());
    }

    private static void object(Writer writer, ObjectFence value) {
        Writer selected = new Writer();
        if (value.kind() == 1 || value.kind() == 2) {
            selected.text8(value.objectId());
            selected.nonzero(value.objectGeneration());
            selected.nonzero(value.expectedAuthorityOwnerGeneration());
        } else if (value.kind() == 3) {
            selected.text8(value.stableType());
            selected.text8(value.objectId());
            selected.nonzero(value.objectGeneration());
        } else {
            throw invalid("object kind");
        }
        writer.u8(value.kind());
        writer.u16(selected.size());
        writer.raw(selected.bytes());
    }

    private static ObjectFence object(Reader reader) {
        int kind = reader.u8();
        Reader selected = reader.slice(reader.u16());
        ObjectFence value;
        if (kind == 1 || kind == 2) {
            value = new ObjectFence(
                kind,
                selected.text8(),
                "",
                selected.nonzero(),
                selected.nonzero());
        } else if (kind == 3) {
            String stableType = selected.text8();
            String objectId = selected.text8();
            value = new ObjectFence(
                kind,
                objectId,
                stableType,
                selected.nonzero(),
                0);
        } else {
            throw invalid("object kind");
        }
        selected.end();
        return value;
    }

    private static void root(Writer writer, Root value) {
        Writer selected = new Writer();
        if (value != null) {
            selected.text16(value.reference());
            selected.u32(value.checksum());
        }
        writer.u8(value == null ? 0 : 1);
        writer.u16(selected.size());
        writer.raw(selected.bytes());
    }

    private static Root root(Reader reader) {
        boolean present = reader.bool();
        Reader selected = reader.slice(reader.u16());
        Root value = present
            ? new Root(selected.text16(), selected.u32Unsigned())
            : null;
        selected.end();
        return value;
    }

    private static IllegalArgumentException invalid(String field) {
        return new IllegalArgumentException(
            "invalid canonical relocation " + field);
    }

    record Coordinator(
        String ownerId,
        long ownerLeaseGeneration,
        RoutingId nodeRid,
        long nodeGeneration,
        String expectedAuthorityStoreVersion) {
        Coordinator {
            requireText(ownerId, "ownerId");
            requirePositive(ownerLeaseGeneration, "ownerLeaseGeneration");
            Objects.requireNonNull(nodeRid, "nodeRid");
            requirePositive(nodeGeneration, "nodeGeneration");
            requireText(expectedAuthorityStoreVersion,
                "expectedAuthorityStoreVersion");
        }
    }

    record Target(
        RoutingId nodeRid,
        long nodeGeneration,
        String ownerId,
        long ownerLeaseGeneration) {
        Target {
            Objects.requireNonNull(nodeRid, "nodeRid");
            requirePositive(nodeGeneration, "nodeGeneration");
            requireText(ownerId, "ownerId");
            requirePositive(ownerLeaseGeneration, "ownerLeaseGeneration");
        }
    }

    record ObjectFence(
        int kind,
        String objectId,
        String stableType,
        long objectGeneration,
        long expectedAuthorityOwnerGeneration) {
        ObjectFence {
            if (kind < 1 || kind > 3) {
                throw invalid("object kind");
            }
            requireText(objectId, "objectId");
            stableType = stableType == null ? "" : stableType;
            requirePositive(objectGeneration, "objectGeneration");
            if (kind == 3) {
                requireText(stableType, "stableType");
                if (expectedAuthorityOwnerGeneration != 0) {
                    throw invalid("instance owner generation");
                }
            } else {
                requirePositive(expectedAuthorityOwnerGeneration,
                    "expectedAuthorityOwnerGeneration");
                if (!stableType.isEmpty()) {
                    throw invalid("stateful object stable type");
                }
            }
        }
    }

    record Root(String reference, long checksum) {
        Root {
            requireText(reference, "reference");
            if (checksum < 0 || checksum > 0xffff_ffffL) {
                throw invalid("checksum");
            }
        }
    }

    record Prepare(
        UUID id,
        long targetAttemptGeneration,
        Coordinator coordinator,
        Target target,
        int initiatorRole,
        ObjectFence object,
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        Root root,
        long applicationVersion) {
        Prepare {
            requireIdentity(id, targetAttemptGeneration);
            Objects.requireNonNull(coordinator, "coordinator");
            Objects.requireNonNull(target, "target");
            if (initiatorRole != ROLE_SOURCE) {
                throw invalid("prepare sender role");
            }
            Objects.requireNonNull(object, "object");
            Objects.requireNonNull(sourceNodeRid, "sourceNodeRid");
            requirePositive(sourceNodeGeneration, "sourceNodeGeneration");
        }
    }

    record Ready(
        UUID id,
        long targetAttemptGeneration,
        Coordinator coordinator,
        Target target,
        ObjectFence object,
        int senderRole) {
        Ready {
            requireIdentity(id, targetAttemptGeneration);
            Objects.requireNonNull(coordinator, "coordinator");
            Objects.requireNonNull(target, "target");
            Objects.requireNonNull(object, "object");
            if (senderRole != ROLE_TARGET) {
                throw invalid("ready sender role");
            }
        }
    }

    record Data(
        UUID id,
        long targetAttemptGeneration,
        Coordinator coordinator,
        int senderRole,
        ObjectFence object,
        byte[] frozenRecord) {
        Data {
            requireIdentity(id, targetAttemptGeneration);
            Objects.requireNonNull(coordinator, "coordinator");
            if (senderRole != ROLE_SOURCE) {
                throw invalid("data sender role");
            }
            Objects.requireNonNull(object, "object");
            frozenRecord = Objects.requireNonNull(
                frozenRecord, "frozenRecord").clone();
            if (frozenRecord.length == 0) {
                throw invalid("frozen record");
            }
        }

        @Override
        public byte[] frozenRecord() {
            return frozenRecord.clone();
        }
    }

    record Cutover(
        UUID id,
        long targetAttemptGeneration,
        Coordinator coordinator,
        int senderRole,
        ObjectFence object) {
        Cutover {
            requireIdentity(id, targetAttemptGeneration);
            Objects.requireNonNull(coordinator, "coordinator");
            if (senderRole != ROLE_SOURCE) {
                throw invalid("cutover sender role");
            }
            Objects.requireNonNull(object, "object");
        }
    }

    static final int SOURCE = ROLE_SOURCE;
    static final int TARGET = ROLE_TARGET;

    private static void requireIdentity(UUID id, long attempt) {
        Objects.requireNonNull(id, "id");
        if (id.getMostSignificantBits() == 0
            && id.getLeastSignificantBits() == 0) {
            throw invalid("relocation id");
        }
        requirePositive(attempt, "targetAttemptGeneration");
    }

    private static void requirePositive(long value, String field) {
        if (value <= 0) {
            throw invalid(field);
        }
    }

    private static String requireText(String value, String field) {
        if (value == null || value.isBlank() || value.indexOf('\0') >= 0) {
            throw invalid(field);
        }
        return value;
    }

    private static final class Writer {
        private final ByteArrayOutputStream output =
            new ByteArrayOutputStream();

        int size() {
            return output.size();
        }

        byte[] bytes() {
            return output.toByteArray();
        }

        void raw(byte[] value) {
            output.writeBytes(value);
        }

        void u8(long value) {
            if (value < 0 || value > 0xff) {
                throw invalid("u8");
            }
            output.write((int) value);
        }

        void u16(long value) {
            if (value < 0 || value > 0xffff) {
                throw invalid("u16");
            }
            raw(ByteBuffer.allocate(2).order(ByteOrder.BIG_ENDIAN)
                .putShort((short) value).array());
        }

        void u32(long value) {
            if (value < 0 || value > 0xffff_ffffL) {
                throw invalid("u32");
            }
            raw(ByteBuffer.allocate(4).order(ByteOrder.BIG_ENDIAN)
                .putInt((int) value).array());
        }

        void u64(long value) {
            raw(ByteBuffer.allocate(8).order(ByteOrder.BIG_ENDIAN)
                .putLong(value).array());
        }

        void nonzero(long value) {
            requirePositive(value, "nonzero u64");
            u64(value);
        }

        void uuid(UUID value) {
            Objects.requireNonNull(value, "uuid");
            u64(value.getMostSignificantBits());
            u64(value.getLeastSignificantBits());
        }

        void rid(RoutingId value) {
            byte[] bytes = Objects.requireNonNull(value, "rid").toBytes();
            u8(bytes.length);
            raw(bytes);
        }

        void text8(String value) {
            byte[] bytes = text(value, 0xff);
            u8(bytes.length);
            raw(bytes);
        }

        void text16(String value) {
            byte[] bytes = text(value, 0xffff);
            u16(bytes.length);
            raw(bytes);
        }

        private static byte[] text(String value, int max) {
            byte[] bytes = requireText(value, "text")
                .getBytes(StandardCharsets.UTF_8);
            if (bytes.length > max) {
                throw invalid("text");
            }
            return bytes;
        }
    }

    private static final class Reader {
        private final ByteBuffer input;

        Reader(byte[] value) {
            this(ByteBuffer.wrap(Objects.requireNonNull(value, "value"))
                .order(ByteOrder.BIG_ENDIAN));
        }

        private Reader(ByteBuffer value) {
            input = value.order(ByteOrder.BIG_ENDIAN);
        }

        int u8() {
            require(1);
            return Byte.toUnsignedInt(input.get());
        }

        int u16() {
            require(2);
            return Short.toUnsignedInt(input.getShort());
        }

        long u32Unsigned() {
            require(4);
            return Integer.toUnsignedLong(input.getInt());
        }

        long u64() {
            require(8);
            return input.getLong();
        }

        long nonzero() {
            long value = u64();
            if (value == 0) {
                throw invalid("nonzero u64");
            }
            return value;
        }

        boolean bool() {
            int value = u8();
            if (value > 1) {
                throw invalid("bool");
            }
            return value == 1;
        }

        int role() {
            int value = u8();
            if (value < 1 || value > 3) {
                throw invalid("role");
            }
            return value;
        }

        UUID uuid() {
            UUID value = new UUID(u64(), u64());
            if (value.getMostSignificantBits() == 0
                && value.getLeastSignificantBits() == 0) {
                throw invalid("relocation id");
            }
            return value;
        }

        RoutingId rid() {
            int length = u8();
            if (length == 0) {
                throw invalid("rid");
            }
            return RoutingId.from(bytes(length));
        }

        String text8() {
            return text(u8());
        }

        String text16() {
            return text(u16());
        }

        Reader slice(int length) {
            require(length);
            ByteBuffer selected = input.slice();
            selected.limit(length);
            input.position(input.position() + length);
            return new Reader(selected);
        }

        byte[] remainingBytes() {
            return bytes(input.remaining());
        }

        byte[] bytes(int length) {
            require(length);
            byte[] value = new byte[length];
            input.get(value);
            return value;
        }

        void end() {
            if (input.hasRemaining()) {
                throw invalid("trailing bytes");
            }
        }

        private String text(int length) {
            if (length == 0) {
                throw invalid("text");
            }
            try {
                return StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(ByteBuffer.wrap(bytes(length)))
                    .toString();
            } catch (CharacterCodingException failure) {
                throw invalid("utf8");
            }
        }

        private void require(int length) {
            if (length < 0 || input.remaining() < length) {
                throw invalid("truncated");
            }
        }
    }
}

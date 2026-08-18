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

/** Semantic projection of the five canonical relocation controls. */
final class ZLinkCanonicalRelocationProtocol {
    private static final int ROLE_SOURCE = 1;
    private static final int ROLE_TARGET = 2;
    //  Schema bounds relocationLogicalBytes / relocationChunkCount /
    //  relocationChunkBytes (service-wire-v1.schema.json).
    static final long PAYLOAD_TOTAL_LENGTH_BOUND = 274_877_906_944L;
    static final int PAYLOAD_CHUNK_COUNT_BOUND = 4_096;
    static final int CHUNK_DATA_BYTES_BOUND = 67_108_864;

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
        manifest(writer, value.manifest());
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
            manifest(reader),
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

    static byte[] encodeFailed(Failed value) {
        Objects.requireNonNull(value, "value");
        Writer writer = body(ServiceWireConstants.COMMAND_RELOCATION_FAILED);
        relocation(writer, value.id(), value.targetAttemptGeneration());
        coordinator(writer, value.coordinator());
        target(writer, value.target());
        object(writer, value.object());
        writer.u8(value.senderRole());
        writer.u32(value.failureCode());
        return writer.bytes();
    }

    static Failed decodeFailed(byte[] encoded) {
        Reader reader = body(encoded,
            ServiceWireConstants.COMMAND_RELOCATION_FAILED);
        Failed value = new Failed(
            reader.uuid(),
            reader.nonzero(),
            coordinator(reader),
            target(reader),
            object(reader),
            reader.role(),
            reader.u32Unsigned());
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
        writer.u64(value.boundaryRecordCount());
        writer.u32(value.boundaryChecksumCrc32c());
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
            object(reader),
            reader.ordinal(),
            reader.u32Unsigned());
        reader.end();
        return value;
    }

    static byte[] encodeState(State value) {
        Objects.requireNonNull(value, "value");
        Writer writer = body(ServiceWireConstants.COMMAND_RELOCATION_STATE);
        relocation(writer, value.id(), value.targetAttemptGeneration());
        coordinator(writer, value.coordinator());
        writer.u8(value.senderRole());
        object(writer, value.object());
        writer.u32(value.chunkOrdinal());
        byte[] chunk = value.chunkData();
        writer.u32(chunk.length);
        writer.raw(chunk);
        return writer.bytes();
    }

    static State decodeState(byte[] encoded) {
        Reader reader = body(encoded,
            ServiceWireConstants.COMMAND_RELOCATION_STATE);
        State value = new State(
            reader.uuid(),
            reader.nonzero(),
            coordinator(reader),
            reader.role(),
            object(reader),
            reader.u32Unsigned(),
            reader.bytes(chunkLength(reader)));
        reader.end();
        return value;
    }

    private static int chunkLength(Reader reader) {
        long length = reader.u32Unsigned();
        if (length > CHUNK_DATA_BYTES_BOUND) {
            throw invalid("chunk data length");
        }
        return (int) length;
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

    private static void manifest(Writer writer, Manifest value) {
        writer.u64(value.totalLength());
        writer.u32(value.chunkCount());
        writer.u32(value.checksumCrc32c());
    }

    private static Manifest manifest(Reader reader) {
        return new Manifest(
            reader.u64(),
            (int) reader.u32Unsigned(),
            reader.u32Unsigned());
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

    /** Direct-transfer payload manifest carried by PREPARE (spec 28 §4.2). */
    record Manifest(
        long totalLength,
        int chunkCount,
        long checksumCrc32c) {
        Manifest {
            if (totalLength < 0 || totalLength > PAYLOAD_TOTAL_LENGTH_BOUND) {
                throw invalid("payload total length");
            }
            if (chunkCount < 0 || chunkCount > PAYLOAD_CHUNK_COUNT_BOUND) {
                throw invalid("payload chunk count");
            }
            if (checksumCrc32c < 0 || checksumCrc32c > 0xffff_ffffL) {
                throw invalid("payload checksum");
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
        Manifest manifest,
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
            Objects.requireNonNull(manifest, "manifest");
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

    /** Explicit pre-cutover target failure (spec 28 §3.4). */
    record Failed(
        UUID id,
        long targetAttemptGeneration,
        Coordinator coordinator,
        Target target,
        ObjectFence object,
        int senderRole,
        long failureCode) {
        Failed {
            requireIdentity(id, targetAttemptGeneration);
            Objects.requireNonNull(coordinator, "coordinator");
            Objects.requireNonNull(target, "target");
            Objects.requireNonNull(object, "object");
            if (senderRole != ROLE_TARGET) {
                throw invalid("failure sender role");
            }
            if (failureCode <= 0 || failureCode > 0xffff_ffffL) {
                throw invalid("failure code");
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
        ObjectFence object,
        long boundaryRecordCount,
        long boundaryChecksumCrc32c) {
        Cutover {
            requireIdentity(id, targetAttemptGeneration);
            Objects.requireNonNull(coordinator, "coordinator");
            if (senderRole != ROLE_SOURCE) {
                throw invalid("cutover sender role");
            }
            Objects.requireNonNull(object, "object");
            if (boundaryRecordCount < 0) {
                throw invalid("boundary record count");
            }
            if (boundaryChecksumCrc32c < 0
                || boundaryChecksumCrc32c > 0xffff_ffffL) {
                throw invalid("boundary checksum");
            }
        }
    }

    /** One direct-transfer state chunk (spec 28 §4.2, command 52). */
    record State(
        UUID id,
        long targetAttemptGeneration,
        Coordinator coordinator,
        int senderRole,
        ObjectFence object,
        long chunkOrdinal,
        byte[] chunkData) {
        State {
            requireIdentity(id, targetAttemptGeneration);
            Objects.requireNonNull(coordinator, "coordinator");
            if (senderRole != ROLE_SOURCE) {
                throw invalid("state sender role");
            }
            Objects.requireNonNull(object, "object");
            if (chunkOrdinal < 0 || chunkOrdinal > 0xffff_ffffL) {
                throw invalid("chunk ordinal");
            }
            chunkData = Objects.requireNonNull(
                chunkData, "chunkData").clone();
            if (chunkData.length > CHUNK_DATA_BYTES_BOUND) {
                throw invalid("chunk data length");
            }
        }

        @Override
        public byte[] chunkData() {
            return chunkData.clone();
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

        long ordinal() {
            long value = u64();
            if (value < 0) {
                throw invalid("ordinal");
            }
            return value;
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

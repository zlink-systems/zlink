package systems.zlink.framework.runtime.spots;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.UUID;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

/**
 * Semantic projection of canonical maintenance relocation controls.
 *
 * <p>The raw backend performs the exhaustive shared-schema validation. This
 * codec preserves the fields used by the JVM production coordinator and
 * re-emits records in the exact service-wire order.</p>
 */
final class ZLinkCanonicalRelocationProtocol {
    private static final int ROLE_SOURCE = 1;
    private static final int ROLE_TARGET = 2;

    private ZLinkCanonicalRelocationProtocol() {
    }

    static byte[] encodePrepare(Prepare value) {
        Writer writer = body(ServiceWireConstants.COMMAND_RELOCATION_PREPARE, 0);
        common(writer, value.id(), value.attempt(), value.round(),
            value.coordinator(), value.candidate());
        writer.u8(value.initiatorRole());
        object(writer, value.object());
        writer.rid(value.sourceNodeRid());
        writer.u64(value.sourceNodeGeneration());
        writer.u64(value.requiredMessages());
        writer.u64(value.requiredBytes());
        participants(writer, value.participants());
        root(writer, value.root());
        writer.u64(value.applicationVersion());
        return writer.bytes();
    }

    static Prepare decodePrepare(byte[] encoded) {
        Reader reader = body(encoded,
            ServiceWireConstants.COMMAND_RELOCATION_PREPARE, 0);
        UUID id = reader.uuid();
        long attempt = reader.nonzero();
        int round = reader.u8();
        Coordinator coordinator = coordinator(reader);
        Candidate candidate = candidate(reader);
        int role = reader.u8();
        ObjectFence object = object(reader);
        RoutingId sourceRid = reader.rid();
        long sourceGeneration = reader.nonzero();
        long messages = reader.u64();
        long bytes = reader.u64();
        List<Participant> participants = participants(reader);
        Root root = root(reader);
        long applicationVersion = reader.u64();
        reader.end();
        return new Prepare(id, attempt, round, coordinator, candidate, role,
            object, sourceRid, sourceGeneration, messages, bytes,
            participants, root, applicationVersion);
    }

    static byte[] encodeReady(Ready value) {
        Writer writer = body(ServiceWireConstants.COMMAND_RELOCATION_READY,
            ServiceWireConstants.FLAG_EXTENSION);
        common(writer, value.id(), value.attempt(), value.round(),
            value.coordinator(), value.candidate());
        object(writer, value.object());
        writer.u8(value.role());
        writer.u64(value.offeredMessages());
        writer.u64(value.offeredBytes());
        participants(writer, value.participants());
        Writer extension = new Writer();
        extension.tlv64(2, value.sourceNodeGeneration());
        extension.tlv64(3, value.targetNodeGeneration());
        extension.tlv64(4, value.reservationGeneration());
        if (value.root() != null) {
            Writer reference = new Writer();
            reference.text16(value.root().reference());
            extension.tlv(5, reference.bytes());
            Writer checksum = new Writer();
            checksum.u32(value.root().checksum());
            extension.tlv(6, checksum.bytes());
        }
        extension.tlv64(8, value.applicationVersion());
        Writer progress = new Writer();
        progress.u32(value.progress().size());
        for (Progress item : value.progress()) {
            progress.u64(item.participantId());
            progress.u64(item.acceptedBoundary());
            progress.u64(item.replayCursor());
        }
        extension.tlv(9, progress.bytes());
        writer.u32(extension.size());
        writer.raw(extension.bytes());
        return writer.bytes();
    }

    static Ready decodeReady(byte[] encoded) {
        Reader reader = body(encoded,
            ServiceWireConstants.COMMAND_RELOCATION_READY,
            ServiceWireConstants.FLAG_EXTENSION);
        UUID id = reader.uuid();
        long attempt = reader.nonzero();
        int round = reader.u8();
        Coordinator coordinator = coordinator(reader);
        Candidate candidate = candidate(reader);
        ObjectFence object = object(reader);
        int role = reader.u8();
        long offeredMessages = reader.u64();
        long offeredBytes = reader.u64();
        List<Participant> participants = participants(reader);
        Reader extension = reader.slice(reader.u32());
        long sourceGeneration = 0;
        long targetGeneration = 0;
        long reservationGeneration = 0;
        Root root = null;
        long applicationVersion = 0;
        List<Progress> progress = List.of();
        int previous = 0;
        String reference = null;
        Long checksum = null;
        while (extension.remaining() != 0) {
            int tag = extension.u8();
            if (tag <= previous) {
                throw invalid("ready extension order");
            }
            previous = tag;
            Reader field = extension.slice(extension.u32());
            switch (tag) {
                case 2 -> sourceGeneration = field.nonzero();
                case 3 -> targetGeneration = field.nonzero();
                case 4 -> reservationGeneration = field.nonzero();
                case 5 -> reference = field.text16();
                case 6 -> checksum = field.u32Unsigned();
                case 8 -> applicationVersion = field.u64();
                case 9 -> {
                    int count = field.u32();
                    List<Progress> decoded = new ArrayList<>(count);
                    for (int index = 0; index < count; index++) {
                        decoded.add(new Progress(
                            field.nonzero(), field.u64(), field.u64()));
                    }
                    progress = List.copyOf(decoded);
                }
                default -> throw invalid("ready extension tag");
            }
            field.end();
        }
        extension.end();
        reader.end();
        if ((reference == null) != (checksum == null)) {
            throw invalid("ready root pair");
        }
        if (reference != null) {
            root = new Root(reference, checksum);
        }
        return new Ready(id, attempt, round, coordinator, candidate, object,
            role, offeredMessages, offeredBytes, participants,
            sourceGeneration, targetGeneration, reservationGeneration,
            root, applicationVersion, progress);
    }

    static byte[] encodeReserved(Reserved value) {
        Writer writer = body(ServiceWireConstants.COMMAND_RELOCATION_RESERVED, 0);
        common(writer, value.id(), value.attempt(), value.round(),
            value.coordinator(), value.candidate());
        writer.u64(value.reservationGeneration());
        participants(writer, value.participants());
        return writer.bytes();
    }

    static Reserved decodeReserved(byte[] encoded) {
        Reader reader = body(encoded,
            ServiceWireConstants.COMMAND_RELOCATION_RESERVED, 0);
        Reserved value = new Reserved(
            reader.uuid(),
            reader.nonzero(),
            reader.u8(),
            coordinator(reader),
            candidate(reader),
            reader.nonzero(),
            participants(reader));
        reader.end();
        return value;
    }

    static byte[] encodeControlData(ControlData value) {
        Writer writer = body(ServiceWireConstants.COMMAND_RELOCATION_DATA, 0);
        relocationBase(writer, value.id(), value.attempt(), value.coordinator());
        writer.u8(value.senderRole());
        writer.u64(value.participantId());
        writer.u64(value.sequence());
        Writer frozen = new Writer();
        frozen.u8(13);
        frozen.u8(1);
        Writer source = new Writer();
        source.rid(value.source().nodeRid());
        source.u64(value.source().nodeGeneration());
        source.text8(value.source().ownerId());
        source.u64(value.source().ownerLeaseGeneration());
        frozen.u16(source.size());
        frozen.raw(source.bytes());
        frozen.u8(0);
        frozen.u64(0);
        frozen.u64(0);
        frozen.u32(0);
        frozen.u16(0);
        frozen.u8(value.phase());
        frozen.u8(value.senderRole());
        frozen.uuid(value.id());
        object(frozen, value.object());
        frozen.u32(value.terminalResult());
        frozen.u32(value.failureCode());
        writer.raw(frozen.bytes());
        return writer.bytes();
    }

    static ControlData decodeControlData(byte[] encoded) {
        Reader reader = body(encoded,
            ServiceWireConstants.COMMAND_RELOCATION_DATA, 0);
        UUID id = reader.uuid();
        long attempt = reader.nonzero();
        Coordinator coordinator = coordinator(reader);
        int senderRole = reader.u8();
        long participant = reader.nonzero();
        long sequence = reader.nonzero();
        if (reader.u8() != 13 || reader.u8() != 1) {
            throw invalid("relocation control record kind");
        }
        Reader sourceBody = reader.slice(reader.u16());
        Source source = new Source(
            sourceBody.rid(),
            sourceBody.nonzero(),
            sourceBody.text8(),
            sourceBody.nonzero());
        sourceBody.end();
        if (reader.u8() != 0
            || reader.u64() != 0 || reader.u64() != 0
            || reader.u32() != 0 || reader.u16() != 0) {
            throw invalid("relocation control operation");
        }
        int phase = reader.u8();
        int frozenRole = reader.u8();
        UUID frozenId = reader.uuid();
        ObjectFence object = object(reader);
        long terminal = reader.u32Unsigned();
        long failure = reader.u32Unsigned();
        reader.end();
        if (frozenRole != senderRole || !frozenId.equals(id)) {
            throw invalid("relocation control identity");
        }
        return new ControlData(id, attempt, coordinator, senderRole,
            participant, sequence, source, phase, object, terminal, failure);
    }

    static byte[] encodeAck(Ack value) {
        Writer writer = body(ServiceWireConstants.COMMAND_RELOCATION_ACK, 0);
        relocationBase(writer, value.id(), value.attempt(), value.coordinator());
        writer.u8(value.senderRole());
        writer.u64(value.participantId());
        writer.u64(value.highWater());
        return writer.bytes();
    }

    static Ack decodeAck(byte[] encoded) {
        Reader reader = body(encoded,
            ServiceWireConstants.COMMAND_RELOCATION_ACK, 0);
        Ack value = new Ack(
            reader.uuid(),
            reader.nonzero(),
            coordinator(reader),
            reader.u8(),
            reader.nonzero(),
            reader.u64());
        reader.end();
        return value;
    }

    static byte[] encodeSeal(Seal value) {
        Writer writer = body(ServiceWireConstants.COMMAND_RELOCATION_SEAL, 0);
        relocationBase(writer, value.id(), value.attempt(), value.coordinator());
        writer.u8(value.senderRole());
        writer.u8(value.response() ? 1 : 0);
        writer.u32(value.participants().size());
        for (Terminal item : value.participants()) {
            writer.u64(item.participantId());
            writer.u64(item.highWater());
        }
        return writer.bytes();
    }

    static Seal decodeSeal(byte[] encoded) {
        Reader reader = body(encoded,
            ServiceWireConstants.COMMAND_RELOCATION_SEAL, 0);
        UUID id = reader.uuid();
        long attempt = reader.nonzero();
        Coordinator coordinator = coordinator(reader);
        int role = reader.u8();
        boolean response = reader.bool();
        int count = reader.u32();
        List<Terminal> terminals = new ArrayList<>(count);
        for (int index = 0; index < count; index++) {
            terminals.add(new Terminal(reader.nonzero(), reader.u64()));
        }
        reader.end();
        return new Seal(id, attempt, coordinator, role, response, terminals);
    }

    static byte[] encodeComplete(Complete value) {
        Writer writer = body(ServiceWireConstants.COMMAND_RELOCATION_COMPLETE, 0);
        relocationBase(writer, value.id(), value.attempt(), value.coordinator());
        writer.u8(value.senderRole());
        writer.text8(value.source().ownerId());
        writer.u64(value.source().ownerLeaseGeneration());
        writer.rid(value.source().nodeRid());
        writer.u64(value.source().nodeGeneration());
        writer.u8(value.cleanupState());
        return writer.bytes();
    }

    static Complete decodeComplete(byte[] encoded) {
        Reader reader = body(encoded,
            ServiceWireConstants.COMMAND_RELOCATION_COMPLETE, 0);
        UUID id = reader.uuid();
        long attempt = reader.nonzero();
        Coordinator coordinator = coordinator(reader);
        int role = reader.u8();
        String sourceOwnerId = reader.text8();
        long sourceOwnerLeaseGeneration = reader.nonzero();
        RoutingId sourceNodeRid = reader.rid();
        long sourceNodeGeneration = reader.nonzero();
        Source source = new Source(
            sourceNodeRid,
            sourceNodeGeneration,
            sourceOwnerId,
            sourceOwnerLeaseGeneration);
        int cleanup = reader.u8();
        reader.end();
        return new Complete(id, attempt, coordinator, role, source, cleanup);
    }

    private static Writer body(int command, int flags) {
        Writer writer = new Writer();
        writer.u8(ServiceWireConstants.MAGIC_0);
        writer.u8(ServiceWireConstants.MAGIC_1);
        writer.u8(ServiceWireConstants.WIRE_MAJOR);
        writer.u8(command);
        writer.u8(flags);
        return writer;
    }

    private static Reader body(byte[] encoded, int command, int flags) {
        Reader reader = new Reader(encoded);
        if (reader.u8() != ServiceWireConstants.MAGIC_0
            || reader.u8() != ServiceWireConstants.MAGIC_1
            || reader.u8() != ServiceWireConstants.WIRE_MAJOR
            || reader.u8() != command
            || reader.u8() != flags) {
            throw invalid("prefix");
        }
        return reader;
    }

    private static void common(
        Writer writer,
        UUID id,
        long attempt,
        int round,
        Coordinator coordinator,
        Candidate candidate) {
        writer.uuid(id);
        writer.u64(attempt);
        writer.u8(round);
        coordinator(writer, coordinator);
        candidate(writer, candidate);
    }

    private static void relocationBase(
        Writer writer,
        UUID id,
        long attempt,
        Coordinator coordinator) {
        writer.uuid(id);
        writer.u64(attempt);
        coordinator(writer, coordinator);
    }

    private static void coordinator(Writer writer, Coordinator value) {
        writer.text8(value.ownerId());
        writer.u64(value.ownerLeaseGeneration());
        writer.rid(value.nodeRid());
        writer.u64(value.nodeGeneration());
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

    private static void candidate(Writer writer, Candidate value) {
        writer.rid(value.nodeRid());
        writer.u64(value.nodeGeneration());
        writer.text8(value.ownerId());
        writer.u64(value.ownerLeaseGeneration());
    }

    private static Candidate candidate(Reader reader) {
        return new Candidate(
            reader.rid(),
            reader.nonzero(),
            reader.text8(),
            reader.nonzero());
    }

    private static void object(Writer writer, ObjectFence value) {
        Writer body = new Writer();
        if (value.kind() == 1 || value.kind() == 2) {
            body.text8(value.objectId());
            body.u64(value.objectGeneration());
            body.u64(value.expectedAuthorityOwnerGeneration());
        } else {
            body.text8(value.stableType());
            body.text8(value.objectId());
            body.u64(value.expectedAuthorityOwnerGeneration());
        }
        writer.u8(value.kind());
        writer.u16(body.size());
        writer.raw(body.bytes());
    }

    private static ObjectFence object(Reader reader) {
        int kind = reader.u8();
        Reader body = reader.slice(reader.u16());
        ObjectFence value;
        if (kind == 1 || kind == 2) {
            value = new ObjectFence(
                kind, body.text8(), "", body.nonzero(), body.nonzero());
        } else if (kind == 3) {
            String stableType = body.text8();
            String objectId = body.text8();
            value = new ObjectFence(
                kind, objectId, stableType, 0, body.nonzero());
        } else {
            throw invalid("object kind");
        }
        body.end();
        return value;
    }

    private static void participants(
        Writer writer, List<Participant> values) {
        writer.u32(values.size());
        for (Participant value : values) {
            writer.u64(value.participantId());
            Writer identity = new Writer();
            if (value.kind() == 2) {
                identity.rid(value.sessionOwnerNodeRid());
                identity.u64(value.sessionOwnerNodeGeneration());
                identity.text8(value.sessionOwnerId());
                identity.u64(value.sessionOwnerLeaseGeneration());
                identity.rid(value.sessionRid());
                identity.u64(value.bindingGeneration());
            }
            writer.u8(value.kind());
            writer.u16(identity.size());
            writer.raw(identity.bytes());
            writer.u64(value.allowanceMessages());
            writer.u64(value.allowanceBytes());
        }
    }

    private static List<Participant> participants(Reader reader) {
        int count = reader.u32();
        List<Participant> values = new ArrayList<>(count);
        for (int index = 0; index < count; index++) {
            long id = reader.nonzero();
            int kind = reader.u8();
            Reader identity = reader.slice(reader.u16());
            RoutingId ownerRid = null;
            long ownerGeneration = 0;
            String ownerId = null;
            long ownerLease = 0;
            RoutingId sessionRid = null;
            long bindingGeneration = 0;
            if (kind == 2) {
                ownerRid = identity.rid();
                ownerGeneration = identity.nonzero();
                ownerId = identity.text8();
                ownerLease = identity.nonzero();
                sessionRid = identity.rid();
                bindingGeneration = identity.nonzero();
            } else if (kind != 1) {
                throw invalid("participant kind");
            }
            identity.end();
            values.add(new Participant(
                id, kind, ownerRid, ownerGeneration, ownerId, ownerLease,
                sessionRid, bindingGeneration, reader.u64(), reader.u64()));
        }
        return List.copyOf(values);
    }

    private static void root(Writer writer, Root root) {
        Writer body = new Writer();
        if (root != null) {
            body.text16(root.reference());
            body.u32(root.checksum());
        }
        writer.u8(root == null ? 0 : 1);
        writer.u16(body.size());
        writer.raw(body.bytes());
    }

    private static Root root(Reader reader) {
        boolean present = reader.bool();
        Reader body = reader.slice(reader.u16());
        Root value = present
            ? new Root(body.text16(), body.u32Unsigned())
            : null;
        body.end();
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
    }

    record Candidate(
        RoutingId nodeRid,
        long nodeGeneration,
        String ownerId,
        long ownerLeaseGeneration) {
    }

    record ObjectFence(
        int kind,
        String objectId,
        String stableType,
        long objectGeneration,
        long expectedAuthorityOwnerGeneration) {
    }

    record Participant(
        long participantId,
        int kind,
        RoutingId sessionOwnerNodeRid,
        long sessionOwnerNodeGeneration,
        String sessionOwnerId,
        long sessionOwnerLeaseGeneration,
        RoutingId sessionRid,
        long bindingGeneration,
        long allowanceMessages,
        long allowanceBytes) {
        static Participant plain(
            long id, long messages, long bytes) {
            return new Participant(
                id, 1, null, 0, null, 0, null, 0, messages, bytes);
        }
    }

    record Root(String reference, long checksum) {
    }

    record Progress(
        long participantId,
        long acceptedBoundary,
        long replayCursor) {
    }

    record Prepare(
        UUID id,
        long attempt,
        int round,
        Coordinator coordinator,
        Candidate candidate,
        int initiatorRole,
        ObjectFence object,
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        long requiredMessages,
        long requiredBytes,
        List<Participant> participants,
        Root root,
        long applicationVersion) {
        Prepare {
            participants = List.copyOf(participants);
        }
    }

    record Ready(
        UUID id,
        long attempt,
        int round,
        Coordinator coordinator,
        Candidate candidate,
        ObjectFence object,
        int role,
        long offeredMessages,
        long offeredBytes,
        List<Participant> participants,
        long sourceNodeGeneration,
        long targetNodeGeneration,
        long reservationGeneration,
        Root root,
        long applicationVersion,
        List<Progress> progress) {
        Ready {
            participants = List.copyOf(participants);
            progress = List.copyOf(progress);
        }
    }

    record Reserved(
        UUID id,
        long attempt,
        int round,
        Coordinator coordinator,
        Candidate candidate,
        long reservationGeneration,
        List<Participant> participants) {
        Reserved {
            participants = List.copyOf(participants);
        }
    }

    record Source(
        RoutingId nodeRid,
        long nodeGeneration,
        String ownerId,
        long ownerLeaseGeneration) {
    }

    record ControlData(
        UUID id,
        long attempt,
        Coordinator coordinator,
        int senderRole,
        long participantId,
        long sequence,
        Source source,
        int phase,
        ObjectFence object,
        long terminalResult,
        long failureCode) {
    }

    record Ack(
        UUID id,
        long attempt,
        Coordinator coordinator,
        int senderRole,
        long participantId,
        long highWater) {
    }

    record Terminal(long participantId, long highWater) {
    }

    record Seal(
        UUID id,
        long attempt,
        Coordinator coordinator,
        int senderRole,
        boolean response,
        List<Terminal> participants) {
        Seal {
            participants = List.copyOf(participants);
        }
    }

    record Complete(
        UUID id,
        long attempt,
        Coordinator coordinator,
        int senderRole,
        Source source,
        int cleanupState) {
    }

    static final int SOURCE = ROLE_SOURCE;
    static final int TARGET = ROLE_TARGET;

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
            if (value < 0 || value > 0xff) throw invalid("u8");
            output.write((int) value);
        }

        void u16(long value) {
            if (value < 0 || value > 0xffff) throw invalid("u16");
            raw(ByteBuffer.allocate(2).order(ByteOrder.BIG_ENDIAN)
                .putShort((short) value).array());
        }

        void u32(long value) {
            if (value < 0 || value > 0xffff_ffffL) throw invalid("u32");
            raw(ByteBuffer.allocate(4).order(ByteOrder.BIG_ENDIAN)
                .putInt((int) value).array());
        }

        void u64(long value) {
            raw(ByteBuffer.allocate(8).order(ByteOrder.BIG_ENDIAN)
                .putLong(value).array());
        }

        void uuid(UUID value) {
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

        void tlv(int tag, byte[] value) {
            u8(tag);
            u32(value.length);
            raw(value);
        }

        void tlv64(int tag, long value) {
            Writer body = new Writer();
            body.u64(value);
            tlv(tag, body.bytes());
        }

        private static byte[] text(String value, int max) {
            byte[] bytes = Objects.requireNonNull(value, "text")
                .getBytes(StandardCharsets.UTF_8);
            if (bytes.length == 0 || bytes.length > max
                || value.indexOf('\0') >= 0) {
                throw invalid("text");
            }
            return bytes;
        }
    }

    private static final class Reader {
        private final ByteBuffer input;

        Reader(byte[] value) {
            this(ByteBuffer.wrap(
                Objects.requireNonNull(value, "value"))
                .order(ByteOrder.BIG_ENDIAN));
        }

        private Reader(ByteBuffer value) {
            input = value.order(ByteOrder.BIG_ENDIAN);
        }

        int remaining() {
            return input.remaining();
        }

        int u8() {
            require(1);
            return Byte.toUnsignedInt(input.get());
        }

        int u16() {
            require(2);
            return Short.toUnsignedInt(input.getShort());
        }

        int u32() {
            long value = u32Unsigned();
            if (value > Integer.MAX_VALUE) throw invalid("length");
            return (int) value;
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
            if (value == 0) throw invalid("nonzero u64");
            return value;
        }

        boolean bool() {
            int value = u8();
            if (value > 1) throw invalid("bool");
            return value == 1;
        }

        UUID uuid() {
            UUID value = new UUID(u64(), u64());
            if (value.equals(new UUID(0, 0))) throw invalid("relocation id");
            return value;
        }

        RoutingId rid() {
            int length = u8();
            if (length == 0) throw invalid("rid");
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
            ByteBuffer slice = input.slice();
            slice.limit(length);
            input.position(input.position() + length);
            return new Reader(slice);
        }

        byte[] bytes(int length) {
            require(length);
            byte[] value = new byte[length];
            input.get(value);
            return value;
        }

        void end() {
            if (input.hasRemaining()) throw invalid("trailing bytes");
        }

        private String text(int length) {
            if (length == 0) throw invalid("text");
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
